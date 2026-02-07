# MCP 和 Skills 规范（当前实现）

## 1. 分层原则

- MCP 工具层：提供原子、可校验、可审计的硬件能力。
- Skills 层：编排多个 MCP 工具形成业务流程。
- 对话层（WebUI/Router）：只负责路由与交互，不承载业务状态。

## 2. MCP 工具分类

### 2.1 只读工具（Read）
- `get_hardware_topology_mcp`
- `check_serial_config_mcp`
- `detect_and_connect_mcp`
- `read_water_level_mcp`
- `read_temperature_mcp`
- `read_humidity_mcp`
- `read_environment_snapshot_mcp`
- `read_vm_state_mcp`
- `evaluate_environment_thresholds_mcp`
- `device_self_check_mcp`
- `mcp_health_report_mcp`
- `get_guard_status_mcp`
- `get_recent_audit_events_mcp`

### 2.2 写操作工具（Write）
- `relay_set_mcp`
- `relay_all_off_mcp`
- `run_safety_control_mcp`
- `reset_guard_mcp`
- `execute_m_logic_mcp`（低层调试能力，建议受限使用）

## 3. Skills 入口（工作流）

- `run_patrol_skill_mcp`
  - 设备巡检：自检 + 健康报告 + 最近审计
- `run_environment_skill_mcp`
  - 环境评估：快照 + 阈值告警
- `run_safe_control_skill_mcp`
  - 安全控制：auto/emergency_stop/pulse

## 4. 安全策略

- 高风险操作要求确认参数（`safety_confirm`）。
- 继电器持续开启受时长上限保护（`MCP_RELAY_MAX_DURATION_SEC`）。
- 熔断保护：连续失败后进入冷却期，返回 `CIRCUIT_OPEN`。
- 审计落盘：每次工具调用写入 `data/mcp/audit.jsonl`。

## 5. 关键环境变量

- `MCP_SERIAL_PORT` / `MCP_BAUD`
- `MCP_RELAY_MAX_DURATION_SEC`
- `MCP_CIRCUIT_FAIL_THRESHOLD`
- `MCP_CIRCUIT_COOLDOWN_SEC`
- `MCP_READ_RETRY_COUNT`
- `MCP_WRITE_RETRY_COUNT`
- `MCP_AUDIT_DIR`

## 6. 开发约定

- 新增硬件能力：先加 MCP 语义工具，再接入 Skills。
- 新增业务流程：仅在 `python/MCP/skills.py` 编排，不把流程写死到 Router。
- 变更后必须做：
  - Python 编译检查
  - 至少一次真实串口链路验证
  - 审计记录验证

## 7. 推荐测试顺序

1. `device_self_check_mcp`
2. `run_patrol_skill_mcp`
3. `run_environment_skill_mcp`
4. `run_safe_control_skill_mcp`（先 `strategy=auto`，再按需 `pulse`）
5. 失败时用 `get_guard_status_mcp` + `get_recent_audit_events_mcp` 排查
