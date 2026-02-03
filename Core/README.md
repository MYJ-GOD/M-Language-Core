# M-Language Core (MVM)

M 语言的虚拟机实现，栈机架构，Core 指令集 (0-99)。

## 文件结构

```
Core/
├── include/
│   ├── disasm.h      # 反汇编器接口
│   ├── m_vm.h       # VM 核心头文件
│   └── validator.h   # 静态验证器接口
├── src/
│   ├── disasm.c     # 反汇编器实现
│   ├── m_vm.c       # VM 核心实现
│   ├── main.c       # 测试套件入口
│   └── validator.c  # 静态验证器实现
├── README.md
├── M-Language-Core.sln
└── M-Language-Core.vcxproj
```

## 构建

### Windows (Visual Studio)

```bash
# 使用 MSBuild
MSBuild.exe M-Language-Core.sln /p:Configuration=Debug /p:Platform=x64

# 或直接在 Visual Studio 中打开 .sln 文件
```

### Linux/macOS (GCC/Clang)

```bash
gcc -o mvm src/m_vm.c src/disasm.c src/main.c src/validator.c -I include -O2
./mvm
```

## 快速测试

```bash
# 运行所有测试
./x64/Debug/M-Language-Core.exe
```

## 相关文档

- [M-Token 规范](../Docs/M-Token规范.md)
- [MCP Server](../MCP/README.md)

