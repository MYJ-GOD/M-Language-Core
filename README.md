# M-Language Core

M-Language Core 是一个面向 AI 与硬件控制的本地系统，当前已实现：
- 本地 Ollama + Tool Router + MCP Server + Open WebUI 全链路
- 传感器读取（水位/温度/湿度）
- 继电器安全控制（确认门禁、自动回落）
- 审计日志、熔断保护、健康报告
- Skills 编排层（巡检/环境评估/安全控制）
- M-IR 编译链（自然语言 → M-IR → M-Token 字节码 → 执行）

## 1. 项目结构

```text
M-Language-Core/
├── src/                          # C 源码（MVM 虚拟机）
│   ├── include/                  # 头文件
│   │   ├── m_vm.h
│   │   ├── disasm.h
│   │   └── validator.h
│   ├── m_vm.c                    # 虚拟机核心
│   ├── disasm.c                  # 反汇编器
│   ├── validator.c               # 字节码验证器
│   └── main.c                    # 测试套件
├── firmware/
│   └── mvm_esp8266.ino           # ESP8266 固件
├── python/
│   ├── mcp/                      # MCP 服务器 + Skills
│   │   ├── server.py             # MCP 工具层（25+ 工具）
│   │   ├── skills.py             # Skills 编排层（4 个工作流）
│   │   ├── core.py               # 串口通信封装
│   │   ├── lir_backend.py        # M-IR 编译桥接
│   │   ├── pc_resources.py       # PC 资源设备化
│   │   └── mlang/                # M-IR 编译器 + 模拟器
│   │       ├── compiler.py
│   │       └── simulator.py
│   └── tool_router/              # OpenAI-compatible Router
│       └── router.py
├── tests/                        # 测试
│   ├── test_lir_backend.py       # M-IR 后端回归测试（15 用例）
│   └── smoke_e2e.py              # 端到端冒烟测试
├── scripts/
│   └── start_all.bat             # 一键启动脚本
├── docs/                         # 文档
│   ├── 00_文档导航.md
│   ├── 40_施工路线图.md
│   ├── 10_MCP+Skills实施方案.md
│   ├── mcp和skills.md
│   ├── M-Token规范.md
│   ├── M 语言体系完整大纲.md
│   ├── 20_AI原生操作系统架构.md
│   └── 30_项目完成度报告.md
├── data/                         # 运行时数据（审计、缓存等）
├── .gitignore
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
- `relay_set_with_verify_mcp`
- `relay_all_off_mcp`
- `run_safety_control_mcp`
- `reset_guard_mcp`
- `execute_m_logic_mcp`（低层调试）

M-IR 编译链：
- `execute_lir_mcp`（纯模拟、零副作用）
- `execute_lir_action_mcp`（真实文件写入）

闭环/实验类：
- `read_relay_state_mcp`
- `run_relay_closed_loop_skill_mcp`
- `set_fault_injection_mcp`
- `get_fault_injection_status_mcp`
- `run_experiment_batch_mcp`

### 2.2 Skills 层（编排入口）
- `run_patrol_skill_mcp`：巡检工作流（自检 + 健康报告 + 审计摘要）
- `run_environment_skill_mcp`：环境评估工作流（快照 + 阈值评估）
- `run_safe_control_skill_mcp`：安全控制工作流（auto/emergency_stop/pulse）
- `run_relay_closed_loop_skill_mcp`：继电器闭环控制

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
cd python\mcp
pip install -r requirements.txt
python server.py
```

### 4.2 单独运行 Router

```powershell
cd python\tool_router
pip install -r requirements.txt
uvicorn router:app --host 127.0.0.1 --port 8000
```

### 4.3 运行测试

```powershell
cd tests
python -m pytest test_lir_backend.py -v
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
- M-IR 文件槽：`MCP_LIR_FILE_ROOT`（默认 `data/lir_sandbox`）

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
1. `docs/00_文档导航.md`
2. `docs/40_施工路线图.md`
3. `docs/10_MCP+Skills实施方案.md`
4. `docs/mcp和skills.md`
5. `docs/M-Token规范.md`
6. `docs/M 语言体系完整大纲.md`

## 8. 维护约定

- 新增硬件能力：先做 MCP 语义工具，再接入 Skills。
- 新增业务流程：优先改 `python/mcp/skills.py`，避免把流程逻辑写死在 Router。
- 根目录文档用于草稿，定稿后归档到 `docs/`。

## 许可证

MIT
