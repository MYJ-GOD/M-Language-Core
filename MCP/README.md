# M-Language MCP Server

Python 实现的 MCP Server，用于 AI 与 ESP8266 硬件通信。

## 功能

- **Skill A**: `get_hardware_topology` - 获取设备清单
- **Skill B**: `execute_m_logic` - 下发 M-Token 执行
- **Skill C**: `read_vm_state` - 读取执行状态

## 安装

```bash
cd MCP
pip install -r requirements.txt
```

## 使用

```bash
python server.py --port COM3
```

## 文件结构

```
MCP/
├── server.py         # MCP Server 主程序
├── skills.py         # 3 个核心 Skills 实现
├── protocol.md       # 串口通信协议
├── requirements.txt  # Python 依赖
└── README.md
```

## 相关文档

- [MCP & Skills 规范](../Docs/mcp和skills.md)
- [M-Token 规范](../Docs/M-Token规范.md)

