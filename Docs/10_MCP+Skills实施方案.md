# MCP + Skills 实施方案

> 本文档记录早期实施思路。Phase 0/1/2/3 已完成，当前施工路线见 `Docs/40_施工路线图.md`。

## 1. 分层设计（已实现）

- L0 设备层：ESP8266 + 传感器 + 继电器
- L1 协议层：M-Token + 串口通信（`core.py`）
- L2 工具层（MCP）：提供可调用原子能力
- L3 技能层（Skills）：把多个工具组合成业务流程
- L4 对话层：WebUI + Router + 本地模型

## 2. 已完成的能力

### 2.1 MCP 工具层（25+ 工具）

只读类：`get_hardware_topology_mcp`、`read_water_level_mcp`、`read_temperature_mcp`、`read_humidity_mcp`、`read_environment_snapshot_mcp`、`read_vm_state_mcp`、`device_self_check_mcp`、`evaluate_environment_thresholds_mcp`、`mcp_health_report_mcp`、`get_guard_status_mcp`、`get_recent_audit_events_mcp`

写操作类：`relay_set_mcp`、`relay_set_with_verify_mcp`、`relay_all_off_mcp`、`run_safety_control_mcp`、`reset_guard_mcp`、`execute_m_logic_mcp`

M-IR 编译链：`execute_lir_mcp`（纯模拟）、`execute_lir_action_mcp`（真实文件写入）

### 2.2 Skills 层（4 个工作流）

- `run_patrol_skill_mcp`：巡检工作流
- `run_environment_skill_mcp`：环境评估工作流
- `run_safe_control_skill_mcp`：安全控制工作流
- `run_relay_closed_loop_skill_mcp`：继电器闭环控制

### 2.3 安全机制

- 熔断保护：连续失败阈值 + 冷却时间
- 审计日志：所有操作记录到 `data/mcp/audit.jsonl`
- 继电器安全：最大持续时间 + 确认门禁 + 自动回落
- M-IR 沙箱：路径穿越拦截 + 两阶段执行（先模拟后物化）

## 3. 设计原则

- MCP 负责"原子能力 + 边界校验"
- Skills 负责"流程编排 + 条件判断 + 重试 + 输出结论"
- 模型不要直接控制底层字节码细节，优先通过语义化 skill 调用
- 新增硬件能力：先做 MCP 语义工具，再接入 Skills
- 新增业务流程：优先改 `skills.py`，避免把流程逻辑写死在 Router
