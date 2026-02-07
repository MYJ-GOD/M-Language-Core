# M-Language Core

M-Language Core 是一个面向 AI 与硬件控制的本地系统，当前已实现：
- 本地 Ollama + Tool Router + MCP Server + Open WebUI 全链路
- 传感器读取（水位/温度/湿度）
- 继电器安全控制（确认门禁、自动回落）
- 审计日志、熔断保护、健康报告
- Skills 编排层（巡检/环境评估/安全控制）

## 1. 项目结构（当前实际）

```text
M-Language-Core/
├── include/                      # C 头文件（MVM）
├── src/                          # C 源码（MVM）
├── firmware/                     # ESP8266 固件
├── python/
│   └── MCP/
│       ├── server.py             # MCP Server（工具层 + 运维工具 + skill入口）
│       ├── skills.py             # Skills 编排层实现
│       ├── core.py               # 串口通信与 M-Token 执行封装
│       ├── protocol.md           # 串口协议
│       ├── requirements.txt
│       └── __init__.py
├── tool-router/
│   ├── router.py                 # OpenAI-compatible Router（连接 Ollama 与 MCP）
│   ├── requirements.txt
│   └── README.md
├── scripts/
│   └── start_all.bat             # 一键启动脚本
├── Docs/
│   ├── 00_文档导航.md
│   ├── 10_MCP+Skills实施方案.md
│   ├── mcp和skills.md
│   ├── M-Token规范.md
│   └── M 语言体系完整大纲.md
├── data/                         # 运行时数据（审计、缓存等）
├── M-Language-Core.sln
└── README.md
```

## 2. 当前能力

### 2.1 MCP 工具层
只读类：
- `get_hardware_topology_mcp`
- `check_serial_config_mcp`
- `detect_and_connect_mcp`
- `read_water_level_mcp`
- `read_temperature_mcp`
- `read_humidity_mcp`
- `read_environment_snapshot_mcp`
- `read_vm_state_mcp`
- `device_self_check_mcp`
- `evaluate_environment_thresholds_mcp`
- `mcp_health_report_mcp`
- `get_guard_status_mcp`
- `get_recent_audit_events_mcp`

写操作类：
- `relay_set_mcp`
- `relay_all_off_mcp`
- `run_safety_control_mcp`
- `reset_guard_mcp`
- `execute_m_logic_mcp`（低层调试）

### 2.2 Skills 层（编排入口）
- `run_patrol_skill_mcp`：巡检工作流（自检 + 健康报告 + 审计摘要）
- `run_environment_skill_mcp`：环境评估工作流（快照 + 阈值评估）
- `run_safe_control_skill_mcp`：安全控制工作流（auto/emergency_stop/pulse）

## 3. 一键启动

直接运行：

```powershell
scripts\start_all.bat
```

脚本会按顺序启动：
1. Ollama
2. MCP Server
3. Tool Router
4. Open WebUI

默认地址：
- Ollama: `http://127.0.0.1:11434`
- Tool Router: `http://127.0.0.1:8000/v1`
- Open WebUI: `http://127.0.0.1:8080`
- MCP Server: `http://127.0.0.1:9001/mcp`

## 4. 开发模式

### 4.1 单独运行 MCP Server

```powershell
cd python\MCP
pip install -r requirements.txt
python server.py
```

### 4.2 单独运行 Router

```powershell
cd tool-router
pip install -r requirements.txt
uvicorn router:app --host 127.0.0.1 --port 8000
```

## 5. 关键运行配置（环境变量）

- 串口：`MCP_SERIAL_PORT`、`MCP_BAUD`
- 继电器安全：`MCP_RELAY_MAX_DURATION_SEC`
- 守护策略：
  - `MCP_CIRCUIT_FAIL_THRESHOLD`
  - `MCP_CIRCUIT_COOLDOWN_SEC`
  - `MCP_READ_RETRY_COUNT`
  - `MCP_WRITE_RETRY_COUNT`
- 审计：`MCP_AUDIT_DIR`（默认 `data/mcp`）

## 6. 运维与排障

建议排障顺序：
1. `mcp_health_report_mcp`
2. `get_guard_status_mcp`
3. `get_recent_audit_events_mcp`
4. 必要时 `reset_guard_mcp`

审计日志位置：
- `data/mcp/audit.jsonl`

## 7. 文档导航

请先读：
1. `Docs/00_文档导航.md`
2. `Docs/10_MCP+Skills实施方案.md`
3. `Docs/mcp和skills.md`
4. `Docs/M-Token规范.md`
5. `Docs/M 语言体系完整大纲.md`

## 8. 维护约定

- 新增硬件能力：先做 MCP 语义工具，再接入 Skills。
- 新增业务流程：优先改 `python/MCP/skills.py`，避免把流程逻辑写死在 Router。
- 根目录文档用于草稿，定稿后归档到 `Docs/`。

## 许可证

MIT
