# M-Language MCP Server
# FastMCP 工具定义层 - 只负责暴露接口，不含业务逻辑

import asyncio
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from threading import Lock
from typing import Dict, Any, List

from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings
from skills import SkillEngine
from core import (
    get_hardware_topology,
    execute_m_logic,
    read_vm_state,
    detect_serial_ports
)

_host = os.getenv("MCP_HTTP_HOST", "127.0.0.1")
_port = int(os.getenv("MCP_HTTP_PORT", "9001"))
_relay_max_duration_sec = int(os.getenv("MCP_RELAY_MAX_DURATION_SEC", "60"))
_circuit_fail_threshold = int(os.getenv("MCP_CIRCUIT_FAIL_THRESHOLD", "3"))
_circuit_cooldown_sec = int(os.getenv("MCP_CIRCUIT_COOLDOWN_SEC", "15"))
_read_retry_count = int(os.getenv("MCP_READ_RETRY_COUNT", "1"))
_write_retry_count = int(os.getenv("MCP_WRITE_RETRY_COUNT", "0"))
_audit_dir = Path(os.getenv("MCP_AUDIT_DIR", "data/mcp"))
_audit_file = _audit_dir / "audit.jsonl"

_WATER_DEVICE_ID = 1
_TEMP_DEVICE_ID = 2
_HUMIDITY_DEVICE_ID = 3
_RELAY1_DEVICE_ID = 5
_RELAY2_DEVICE_ID = 6

_circuit_lock = Lock()
_audit_lock = Lock()
_circuit_state: Dict[str, Dict[str, Any]] = {}

# Disable DNS rebinding protection for container-to-container access.
app = FastMCP(
    "m-language-mcp",
    host=_host,
    port=_port,
    transport_security=TransportSecuritySettings(enable_dns_rebinding_protection=False),
)


def _ensure_serial_port() -> str:
    """Ensure serial port is configured, try auto-detection if missing."""
    import serial.tools.list_ports

    port = os.getenv("MCP_SERIAL_PORT", "")
    if port:
        return port

    try:
        ports = list(serial.tools.list_ports.comports())
    except Exception:
        ports = []

    if not ports:
        return ""

    port = ports[0].device
    os.environ["MCP_SERIAL_PORT"] = port
    return port


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _audit_event(event_type: str, payload: Dict[str, Any]) -> None:
    """Append one structured audit event as JSONL."""
    try:
        _audit_dir.mkdir(parents=True, exist_ok=True)
        rec = {
            "ts": _now_iso(),
            "event_type": event_type,
            **payload,
        }
        line = json.dumps(rec, ensure_ascii=False)
        with _audit_lock:
            with _audit_file.open("a", encoding="utf-8") as f:
                f.write(line + "\n")
    except Exception:
        # Audit failure must not break tool execution.
        pass


def _audit_tool_call(tool: str, request: Dict[str, Any], response: Dict[str, Any]) -> None:
    _audit_event(
        "tool_call",
        {
            "tool": tool,
            "request": request,
            "status": response.get("status"),
            "completed": response.get("completed"),
            "code": response.get("code"),
            "fault": response.get("fault"),
            "fault_code": response.get("fault_code"),
            "port_used": response.get("port_used"),
        },
    )


def _circuit_key(op: str, device_id: int) -> str:
    return f"{op}:{device_id}"


def _circuit_check(op: str, device_id: int) -> Dict[str, Any] | None:
    key = _circuit_key(op, device_id)
    now = time.time()
    with _circuit_lock:
        st = _circuit_state.setdefault(key, {"failures": 0, "open_until": 0.0})
        open_until = float(st.get("open_until", 0.0))
        if open_until <= now:
            st["open_until"] = 0.0
            return None
        retry_after = max(1, int(round(open_until - now)))
    return {
        "completed": False,
        "status": "failed",
        "code": "CIRCUIT_OPEN",
        "fault": "CIRCUIT_OPEN",
        "fault_code": 1001,
        "error": f"circuit open for {key}",
        "retry_after_sec": retry_after,
    }


def _circuit_on_success(op: str, device_id: int) -> None:
    key = _circuit_key(op, device_id)
    with _circuit_lock:
        st = _circuit_state.setdefault(key, {"failures": 0, "open_until": 0.0})
        st["failures"] = 0
        st["open_until"] = 0.0


def _circuit_on_failure(op: str, device_id: int, reason: str) -> None:
    key = _circuit_key(op, device_id)
    now = time.time()
    opened = False
    with _circuit_lock:
        st = _circuit_state.setdefault(key, {"failures": 0, "open_until": 0.0})
        st["failures"] = int(st.get("failures", 0)) + 1
        if st["failures"] >= _circuit_fail_threshold:
            st["open_until"] = now + _circuit_cooldown_sec
            st["failures"] = 0
            opened = True
    if opened:
        _audit_event(
            "circuit_opened",
            {
                "key": key,
                "reason": reason,
                "cooldown_sec": _circuit_cooldown_sec,
            },
        )


def _execute_with_guard(
    op: str,
    device_id: int,
    m_tokens: List[int],
    timeout_ms: int,
    retry_count: int,
) -> Dict[str, Any]:
    blocked = _circuit_check(op, device_id)
    if blocked is not None:
        return blocked

    last: Dict[str, Any] = {}
    max_attempts = max(1, retry_count + 1)
    for attempt in range(1, max_attempts + 1):
        last = execute_m_logic(m_tokens, timeout_ms=timeout_ms)
        last["attempt"] = attempt
        if last.get("completed"):
            _circuit_on_success(op, device_id)
            return last

    reason = str(last.get("fault") or last.get("error") or "unknown")
    _circuit_on_failure(op, device_id, reason=reason)
    return last


def _read_device(device_id: int, timeout_ms: int = 5000) -> Dict[str, Any]:
    """Read one device value through M-Token IOR."""
    m_tokens = [80, device_id, 71, device_id, 82]
    return _execute_with_guard(
        op="read",
        device_id=device_id,
        m_tokens=m_tokens,
        timeout_ms=timeout_ms,
        retry_count=_read_retry_count,
    )


def _write_device(device_id: int, value: int, timeout_ms: int = 5000) -> Dict[str, Any]:
    """Write one device value through M-Token IOW."""
    m_tokens = [80, device_id, 30, value, 70, device_id, 82]
    return _execute_with_guard(
        op="write",
        device_id=device_id,
        m_tokens=m_tokens,
        timeout_ms=timeout_ms,
        retry_count=_write_retry_count,
    )


def _decorate_result(raw: Dict[str, Any], port: str, op: str, device_id: int) -> Dict[str, Any]:
    """Attach common metadata for MCP tool responses."""
    out = dict(raw)
    out["op"] = op
    out["device_id"] = device_id
    out["port_used"] = port
    out["status"] = "success" if out.get("completed") else "failed"
    if not out.get("completed"):
        out["hint"] = (
            f"设备 {device_id} 在串口 {port} 执行失败，请检查连接、供电和固件。"
        )
    return out


def _quality_flag(values: List[int], tolerance: int) -> str:
    """Simple stability flag for sampled values."""
    if len(values) <= 1:
        return "unknown"
    spread = max(values) - min(values)
    return "stable" if spread <= tolerance else "noisy"


@app.tool()
def get_hardware_topology_mcp() -> Dict[str, Any]:
    """查看 ESP8266 上连接了哪些硬件设备。

    返回设备列表，包括：
    - 水位传感器 (device_id=1, A0引脚, 读数值 0-1024)
    - 温度传感器 (device_id=2, D4引脚)
    - 湿度传感器 (device_id=3, D4引脚)
    - 继电器1 (device_id=5, D1引脚, 写入 0=关闭, 1=开启)
    - 继电器2 (device_id=6, D2引脚, 写入 0=关闭, 1=开启)

    不需要调用此工具也能控制硬件，工具描述中已有设备 ID 列表。"""
    out = get_hardware_topology()
    _audit_tool_call("get_hardware_topology_mcp", {}, {"status": "success", **out})
    return out


@app.tool()
def read_water_level_mcp() -> Dict[str, Any]:
    """直接读取水位传感器，自动处理串口配置。"""
    port = _ensure_serial_port()
    if not port:
        out = {
            "status": "error",
            "error": "未检测到任何串口设备",
            "hint": "请确保 ESP8266 已通过 USB 连接到电脑",
            "solution": "1. 检查 USB 连接\n2. 安装 CH340 驱动\n3. 重试",
        }
        _audit_tool_call("read_water_level_mcp", {}, out)
        return out
    raw = _read_device(_WATER_DEVICE_ID, timeout_ms=5000)
    out = _decorate_result(raw, port, "read", _WATER_DEVICE_ID)
    _audit_tool_call("read_water_level_mcp", {"timeout_ms": 5000}, out)
    return out


@app.tool()
def read_temperature_mcp(timeout_ms: int = 5000) -> Dict[str, Any]:
    """读取温度传感器（device_id=2，单位摄氏度）。"""
    port = _ensure_serial_port()
    if not port:
        out = {"status": "error", "error": "未检测到串口设备", "code": "PORT_MISSING"}
        _audit_tool_call("read_temperature_mcp", {"timeout_ms": timeout_ms}, out)
        return out
    raw = _read_device(_TEMP_DEVICE_ID, timeout_ms=timeout_ms)
    out = _decorate_result(raw, port, "read", _TEMP_DEVICE_ID)
    if out.get("completed"):
        out["temperature_c"] = out.get("result")
    _audit_tool_call("read_temperature_mcp", {"timeout_ms": timeout_ms}, out)
    return out


@app.tool()
def read_humidity_mcp(timeout_ms: int = 5000) -> Dict[str, Any]:
    """读取湿度传感器（device_id=3，单位百分比）。"""
    port = _ensure_serial_port()
    if not port:
        out = {"status": "error", "error": "未检测到串口设备", "code": "PORT_MISSING"}
        _audit_tool_call("read_humidity_mcp", {"timeout_ms": timeout_ms}, out)
        return out
    raw = _read_device(_HUMIDITY_DEVICE_ID, timeout_ms=timeout_ms)
    out = _decorate_result(raw, port, "read", _HUMIDITY_DEVICE_ID)
    if out.get("completed"):
        out["humidity_pct"] = out.get("result")
    _audit_tool_call("read_humidity_mcp", {"timeout_ms": timeout_ms}, out)
    return out


@app.tool()
def read_environment_snapshot_mcp(
    samples: int = 3,
    sample_interval_ms: int = 200,
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """读取水位+温度+湿度快照，支持短窗口多次采样。"""
    if samples < 1 or samples > 10:
        out = {"status": "error", "error": "samples 必须在 1~10 之间", "code": "BAD_ARG"}
        _audit_tool_call(
            "read_environment_snapshot_mcp",
            {"samples": samples, "sample_interval_ms": sample_interval_ms, "timeout_ms": timeout_ms},
            out,
        )
        return out
    if sample_interval_ms < 0 or sample_interval_ms > 5000:
        out = {"status": "error", "error": "sample_interval_ms 必须在 0~5000 之间", "code": "BAD_ARG"}
        _audit_tool_call(
            "read_environment_snapshot_mcp",
            {"samples": samples, "sample_interval_ms": sample_interval_ms, "timeout_ms": timeout_ms},
            out,
        )
        return out

    port = _ensure_serial_port()
    if not port:
        out = {"status": "error", "error": "未检测到串口设备", "code": "PORT_MISSING"}
        _audit_tool_call(
            "read_environment_snapshot_mcp",
            {"samples": samples, "sample_interval_ms": sample_interval_ms, "timeout_ms": timeout_ms},
            out,
        )
        return out

    water_values: List[int] = []
    temp_values: List[int] = []
    humidity_values: List[int] = []
    errors: List[Dict[str, Any]] = []

    for i in range(samples):
        water = _read_device(_WATER_DEVICE_ID, timeout_ms=timeout_ms)
        temp = _read_device(_TEMP_DEVICE_ID, timeout_ms=timeout_ms)
        humidity = _read_device(_HUMIDITY_DEVICE_ID, timeout_ms=timeout_ms)

        if water.get("completed"):
            water_values.append(int(water.get("result", 0)))
        else:
            errors.append({"sample": i + 1, "device_id": _WATER_DEVICE_ID, "fault": water.get("fault")})

        if temp.get("completed"):
            temp_values.append(int(temp.get("result", 0)))
        else:
            errors.append({"sample": i + 1, "device_id": _TEMP_DEVICE_ID, "fault": temp.get("fault")})

        if humidity.get("completed"):
            humidity_values.append(int(humidity.get("result", 0)))
        else:
            errors.append({"sample": i + 1, "device_id": _HUMIDITY_DEVICE_ID, "fault": humidity.get("fault")})

        if i < samples - 1 and sample_interval_ms > 0:
            time.sleep(sample_interval_ms / 1000.0)

    def _avg(values: List[int]) -> float | None:
        if not values:
            return None
        return round(sum(values) / len(values), 2)

    out = {
        "status": "success" if (water_values and temp_values and humidity_values) else "failed",
        "port_used": port,
        "samples_requested": samples,
        "samples_ok": {
            "water": len(water_values),
            "temperature": len(temp_values),
            "humidity": len(humidity_values),
        },
        "water_level_raw_avg": _avg(water_values),
        "temperature_c_avg": _avg(temp_values),
        "humidity_pct_avg": _avg(humidity_values),
        "water_level_raw_values": water_values,
        "temperature_c_values": temp_values,
        "humidity_pct_values": humidity_values,
        "quality": {
            "water": _quality_flag(water_values, tolerance=50),
            "temperature": _quality_flag(temp_values, tolerance=3),
            "humidity": _quality_flag(humidity_values, tolerance=8),
        },
        "relay_state_supported": False,
        "relay_state_note": "当前固件未实现继电器状态回读，继电器状态需由控制日志推断。",
        "errors": errors,
    }
    _audit_tool_call(
        "read_environment_snapshot_mcp",
        {"samples": samples, "sample_interval_ms": sample_interval_ms, "timeout_ms": timeout_ms},
        out,
    )
    return out


@app.tool()
def relay_set_mcp(
    channel: int,
    state: int,
    duration_sec: int = 0,
    timeout_ms: int = 5000,
    safety_confirm: bool = False,
) -> Dict[str, Any]:
    """控制单路继电器（channel=1/2, state=0/1）。支持自动回落关闭。"""
    if channel not in (1, 2):
        out = {"status": "error", "error": "channel 仅支持 1 或 2", "code": "BAD_ARG"}
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out
    if state not in (0, 1):
        out = {"status": "error", "error": "state 仅支持 0(关) 或 1(开)", "code": "BAD_ARG"}
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out
    if duration_sec < 0:
        out = {"status": "error", "error": "duration_sec 不能为负数", "code": "BAD_ARG"}
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out
    if duration_sec > _relay_max_duration_sec:
        out = {
            "status": "error",
            "error": f"duration_sec 超出上限 {_relay_max_duration_sec}s",
            "code": "SAFETY_LIMIT",
        }
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out
    if state == 1 and duration_sec == 0 and not safety_confirm:
        out = {
            "status": "error",
            "error": "持续开启继电器需要 safety_confirm=true",
            "code": "CONFIRM_REQUIRED",
            "hint": "若只需短时动作，请设置 duration_sec；若确需持续开启，请显式确认。",
        }
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out

    port = _ensure_serial_port()
    if not port:
        out = {"status": "error", "error": "未检测到串口设备", "code": "PORT_MISSING"}
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out

    device_id = _RELAY1_DEVICE_ID if channel == 1 else _RELAY2_DEVICE_ID
    first = _write_device(device_id, state, timeout_ms=timeout_ms)
    out = _decorate_result(first, port, "write", device_id)
    out["channel"] = channel
    out["state_requested"] = state
    out["duration_sec"] = duration_sec

    if not out.get("completed"):
        _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
        return out

    if state == 1 and duration_sec > 0:
        time.sleep(duration_sec)
        back = _write_device(device_id, 0, timeout_ms=timeout_ms)
        out["auto_off"] = {
            "attempted": True,
            "completed": bool(back.get("completed")),
            "fault": back.get("fault"),
            "fault_code": back.get("fault_code"),
        }
        out["final_state"] = 0 if back.get("completed") else 1
        out["status"] = "success" if back.get("completed") else "failed"
        if not back.get("completed"):
            out["hint"] = "自动关闭失败，请立即手动执行 relay_set_mcp(channel, 0)。"
    else:
        out["auto_off"] = {"attempted": False}
        out["final_state"] = state

    _audit_tool_call("relay_set_mcp", {"channel": channel, "state": state, "duration_sec": duration_sec}, out)
    return out


@app.tool()
def relay_all_off_mcp(timeout_ms: int = 5000) -> Dict[str, Any]:
    """关闭两路继电器，作为安全回落操作。"""
    port = _ensure_serial_port()
    if not port:
        out = {"status": "error", "error": "未检测到串口设备", "code": "PORT_MISSING"}
        _audit_tool_call("relay_all_off_mcp", {"timeout_ms": timeout_ms}, out)
        return out

    r1 = _write_device(_RELAY1_DEVICE_ID, 0, timeout_ms=timeout_ms)
    r2 = _write_device(_RELAY2_DEVICE_ID, 0, timeout_ms=timeout_ms)
    ok = bool(r1.get("completed")) and bool(r2.get("completed"))

    out = {
        "status": "success" if ok else "failed",
        "port_used": port,
        "relay_1": _decorate_result(r1, port, "write", _RELAY1_DEVICE_ID),
        "relay_2": _decorate_result(r2, port, "write", _RELAY2_DEVICE_ID),
    }
    _audit_tool_call("relay_all_off_mcp", {"timeout_ms": timeout_ms}, out)
    return out


@app.tool()
def device_self_check_mcp(
    include_relay_write_check: bool = False,
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """执行设备自检：串口、拓扑、传感器采样，并可选继电器写入检查。"""
    serial_cfg = check_serial_config_mcp()
    detected = detect_and_connect_mcp()
    topology = get_hardware_topology_mcp()
    snapshot = read_environment_snapshot_mcp(samples=2, sample_interval_ms=150, timeout_ms=timeout_ms)

    relay_check: Dict[str, Any] = {"skipped": True}
    if include_relay_write_check:
        relay_check = relay_all_off_mcp(timeout_ms=timeout_ms)
        relay_check["skipped"] = False

    checks = {
        "serial_ready": serial_cfg.get("status") == "ok" or bool(os.getenv("MCP_SERIAL_PORT", "")),
        "topology_ready": bool(topology.get("devices")),
        "sensors_ready": snapshot.get("status") == "success",
        "relay_write_ready": relay_check.get("status") == "success" if include_relay_write_check else None,
    }

    if checks["serial_ready"] and checks["topology_ready"] and checks["sensors_ready"]:
        health_status = "ok"
    elif checks["serial_ready"] and checks["topology_ready"]:
        health_status = "degraded"
    else:
        health_status = "failed"

    out = {
        "status": "success" if health_status != "failed" else "failed",
        "health_status": health_status,
        "checks": checks,
        "serial_config": serial_cfg,
        "auto_detect": detected,
        "topology_summary": {
            "controller": topology.get("controller", {}),
            "device_count": len(topology.get("devices", [])),
        },
        "snapshot": snapshot,
        "relay_write_check": relay_check,
        "next_actions": [
            "若 serial_ready=false，先运行 detect_and_connect_mcp 并设置 MCP_SERIAL_PORT。",
            "若 sensors_ready=false，检查传感器接线和供电，确认固件已烧录。",
            "若 relay_write_ready=false，检查继电器模块供电和 IN 引脚接线。",
        ],
    }
    _audit_tool_call(
        "device_self_check_mcp",
        {"include_relay_write_check": include_relay_write_check, "timeout_ms": timeout_ms},
        out,
    )
    return out


@app.tool()
def evaluate_environment_thresholds_mcp(
    water_warn_high: int = 650,
    water_critical_high: int = 800,
    temp_warn_high: int = 35,
    temp_critical_high: int = 45,
    humidity_warn_high: int = 80,
    humidity_critical_high: int = 90,
    samples: int = 3,
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """按阈值评估环境状态，输出 INFO/WARN/CRITICAL。"""
    if not (0 <= water_warn_high <= water_critical_high <= 1024):
        out = {"status": "error", "error": "水位阈值不合法", "code": "BAD_ARG"}
        _audit_tool_call("evaluate_environment_thresholds_mcp", {}, out)
        return out
    if not (-40 <= temp_warn_high <= temp_critical_high <= 125):
        out = {"status": "error", "error": "温度阈值不合法", "code": "BAD_ARG"}
        _audit_tool_call("evaluate_environment_thresholds_mcp", {}, out)
        return out
    if not (0 <= humidity_warn_high <= humidity_critical_high <= 100):
        out = {"status": "error", "error": "湿度阈值不合法", "code": "BAD_ARG"}
        _audit_tool_call("evaluate_environment_thresholds_mcp", {}, out)
        return out

    snapshot = read_environment_snapshot_mcp(
        samples=samples,
        sample_interval_ms=200,
        timeout_ms=timeout_ms,
    )
    if snapshot.get("status") != "success":
        out = {
            "status": "failed",
            "error": "环境快照失败，无法评估阈值",
            "snapshot": snapshot,
        }
        _audit_tool_call("evaluate_environment_thresholds_mcp", {}, out)
        return out

    water = snapshot.get("water_level_raw_avg")
    temp = snapshot.get("temperature_c_avg")
    humidity = snapshot.get("humidity_pct_avg")

    alerts: List[Dict[str, Any]] = []
    level = "INFO"

    def mark(metric: str, value: float, warn: float, critical: float, unit: str) -> None:
        nonlocal level
        if value is None:
            alerts.append({"metric": metric, "level": "WARN", "message": "缺少数据"})
            if level == "INFO":
                level = "WARN"
            return
        if value >= critical:
            alerts.append({
                "metric": metric,
                "level": "CRITICAL",
                "value": value,
                "threshold": critical,
                "unit": unit,
                "message": f"{metric} 达到 CRITICAL 阈值",
            })
            level = "CRITICAL"
        elif value >= warn:
            alerts.append({
                "metric": metric,
                "level": "WARN",
                "value": value,
                "threshold": warn,
                "unit": unit,
                "message": f"{metric} 达到 WARN 阈值",
            })
            if level == "INFO":
                level = "WARN"

    mark("water_level_raw", water, water_warn_high, water_critical_high, "raw_0_1024")
    mark("temperature_c", temp, temp_warn_high, temp_critical_high, "C")
    mark("humidity_pct", humidity, humidity_warn_high, humidity_critical_high, "%")

    if level == "CRITICAL":
        suggestion = "建议立即执行 relay_all_off_mcp()，并检查现场环境。"
    elif level == "WARN":
        suggestion = "建议缩短采样周期并观察趋势，必要时执行安全回落。"
    else:
        suggestion = "环境指标正常。"

    out = {
        "status": "success",
        "overall_level": level,
        "alerts": alerts,
        "snapshot": snapshot,
        "thresholds": {
            "water_warn_high": water_warn_high,
            "water_critical_high": water_critical_high,
            "temp_warn_high": temp_warn_high,
            "temp_critical_high": temp_critical_high,
            "humidity_warn_high": humidity_warn_high,
            "humidity_critical_high": humidity_critical_high,
        },
        "recommendation": suggestion,
    }
    _audit_tool_call("evaluate_environment_thresholds_mcp", {"samples": samples, "timeout_ms": timeout_ms}, out)
    return out


@app.tool()
def run_safety_control_mcp(
    action_mode: str = "none",
    critical_only: bool = True,
    force_action: bool = False,
    require_confirmation: bool = True,
    safety_confirm: bool = False,
    relay_channel: int = 1,
    relay_on_duration_sec: int = 8,
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """安全联动入口：先评估阈值，再按策略执行受控继电器动作。"""
    mode = (action_mode or "none").strip().lower()
    allowed_modes = {"none", "all_off", "pulse_relay"}
    if mode not in allowed_modes:
        out = {
            "status": "error",
            "error": f"action_mode 不支持: {action_mode}",
            "allowed": sorted(allowed_modes),
            "code": "BAD_ARG",
        }
        _audit_tool_call("run_safety_control_mcp", {"action_mode": action_mode}, out)
        return out

    eval_result = evaluate_environment_thresholds_mcp(timeout_ms=timeout_ms)
    if eval_result.get("status") != "success":
        out = {
            "status": "failed",
            "error": "阈值评估失败，未执行任何动作",
            "evaluation": eval_result,
        }
        _audit_tool_call("run_safety_control_mcp", {"action_mode": mode}, out)
        return out

    level = eval_result.get("overall_level", "INFO")
    should_act = level in ("WARN", "CRITICAL")
    if critical_only:
        should_act = level == "CRITICAL"
    if force_action:
        should_act = True

    decision = {
        "mode": mode,
        "critical_only": critical_only,
        "force_action": force_action,
        "require_confirmation": require_confirmation,
        "should_act": should_act,
        "level": level,
    }

    if mode == "none" or not should_act:
        out = {
            "status": "success",
            "evaluation": eval_result,
            "decision": decision,
            "action": {"executed": False, "reason": "策略未触发动作"},
        }
        _audit_tool_call("run_safety_control_mcp", {"action_mode": mode}, out)
        return out

    if require_confirmation and not safety_confirm:
        out = {
            "status": "blocked",
            "evaluation": eval_result,
            "decision": decision,
            "action": {
                "executed": False,
                "reason": "需要人工确认",
                "hint": "如确认执行，请设置 safety_confirm=true。",
            },
        }
        _audit_tool_call("run_safety_control_mcp", {"action_mode": mode}, out)
        return out

    if mode == "all_off":
        action_result = relay_all_off_mcp(timeout_ms=timeout_ms)
    else:  # mode == "pulse_relay"
        action_result = relay_set_mcp(
            channel=relay_channel,
            state=1,
            duration_sec=relay_on_duration_sec,
            timeout_ms=timeout_ms,
            safety_confirm=True,
        )

    out = {
        "status": "success" if action_result.get("status") == "success" else "failed",
        "evaluation": eval_result,
        "decision": decision,
        "action": {
            "executed": True,
            "mode": mode,
            "result": action_result,
        },
    }
    _audit_tool_call("run_safety_control_mcp", {"action_mode": mode}, out)
    return out


@app.tool()
def execute_m_logic_mcp(
    m_tokens: List[int],
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """执行 M-Token 字节码控制硬件。

    参数:
        m_tokens: M-Tokens 列表，如 [80, 1, 71, 1, 82]
        timeout_ms: 超时时间(毫秒)，默认 5000

    返回:
        成功: {"result": 数值, "steps": 执行步数, "completed": true}
        失败: {"fault_code": 错误码, "fault": "错误名", "completed": false}
    """
    out = execute_m_logic(m_tokens, timeout_ms)
    _audit_tool_call("execute_m_logic_mcp", {"m_tokens": m_tokens, "timeout_ms": timeout_ms}, out)
    return out


@app.tool()
def check_serial_config_mcp() -> Dict[str, Any]:
    """检查串口配置。"""
    import os
    port = os.getenv("MCP_SERIAL_PORT", "")
    baud = os.getenv("MCP_BAUD", "115200")

    if not port:
        out = {
            "status": "not_set",
            "error": "MCP_SERIAL_PORT environment variable not set",
            "solution": "Set before running MCP server:",
            "windows": "$env:MCP_SERIAL_PORT='COM3'",
            "linux_mac": "export MCP_SERIAL_PORT='/dev/ttyUSB0'",
            "example_check": "Get-ChildItem Env:MCP_SERIAL_PORT"
        }
        _audit_tool_call("check_serial_config_mcp", {}, out)
        return out

    # 检查端口是否存在（跨平台）
    import os as os_path
    port_exists = False
    if port.startswith("COM") and os.name == "nt":
        # Windows: COM1-COM99
        try:
            com_num = int(port[3:])
            port_exists = 1 <= com_num <= 99
        except:
            pass
    else:
        # Linux/Mac: 检查设备文件
        port_exists = os_path.path.exists(port) or port.startswith("/dev/")

    out = {
        "status": "ok" if port_exists else "port_not_found",
        "MCP_SERIAL_PORT": port,
        "MCP_BAUD": baud,
        "port_exists": port_exists,
        "hint": f"Ensure ESP8266 is connected to {port}" if port_exists else f"Port {port} not found. Check connection."
    }
    _audit_tool_call("check_serial_config_mcp", {}, out)
    return out


@app.tool()
def detect_and_connect_mcp() -> Dict[str, Any]:
    """自动检测并连接 ESP8266 串口。"""
    import os

    # 调用 core 中的串口检测函数
    detected = detect_serial_ports()

    if detected["status"] == "error":
        out = {
            "status": "error",
            "error": detected.get("error", "Unknown error"),
            "solution": "请检查 pyserial 是否已安装: pip install pyserial",
            "windows": "确保 USB 转 TTL 驱动已安装",
            "linux_mac": "检查用户是否有串口权限: sudo usermod -aG dialout $USER"
        }
        _audit_tool_call("detect_and_connect_mcp", {}, out)
        return out

    # 构建快速操作命令
    if detected.get("suggested_port"):
        port = detected["suggested_port"]["device"]
        os_name = os.name
        if os_name == "nt":  # Windows
            quick_action = f"$env:MCP_SERIAL_PORT='{port}'"
        else:  # Linux/Mac
            quick_action = f"export MCP_SERIAL_PORT='{port}'"
    else:
        quick_action = "# 未检测到可用串口，请确认设备已连接"

    out = {
        "status": detected["status"],
        "total_ports": detected.get("total_ports", 0),
        "all_ports": detected.get("all_ports", []),
        "esp8266_candidates": detected.get("esp8266_candidates", []),
        "suggested_port": detected.get("suggested_port"),
        "quick_action": quick_action,
        "hint": detected.get("hint", "")
    }
    _audit_tool_call("detect_and_connect_mcp", {}, out)
    return out


@app.tool()
def read_vm_state_mcp(
    stack_depth: int = 8,
) -> Dict[str, Any]:
    """读取 VM 运行时状态。"""
    out = read_vm_state(stack_depth)
    _audit_tool_call("read_vm_state_mcp", {"stack_depth": stack_depth}, out)
    return out


@app.tool()
def get_guard_status_mcp() -> Dict[str, Any]:
    """查看熔断器状态与守护配置。"""
    now = time.time()
    circuits: Dict[str, Any] = {}
    with _circuit_lock:
        for key, st in _circuit_state.items():
            open_until = float(st.get("open_until", 0.0))
            circuits[key] = {
                "failures": int(st.get("failures", 0)),
                "is_open": open_until > now,
                "retry_after_sec": max(0, int(round(open_until - now))),
            }
    out = {
        "status": "success",
        "config": {
            "circuit_fail_threshold": _circuit_fail_threshold,
            "circuit_cooldown_sec": _circuit_cooldown_sec,
            "read_retry_count": _read_retry_count,
            "write_retry_count": _write_retry_count,
            "audit_file": str(_audit_file),
        },
        "circuits": circuits,
    }
    _audit_tool_call("get_guard_status_mcp", {}, out)
    return out


@app.tool()
def get_recent_audit_events_mcp(limit: int = 50) -> Dict[str, Any]:
    """读取最近审计事件（JSONL）。"""
    if limit < 1 or limit > 1000:
        out = {"status": "error", "error": "limit 必须在 1~1000 之间", "code": "BAD_ARG"}
        _audit_tool_call("get_recent_audit_events_mcp", {"limit": limit}, out)
        return out
    if not _audit_file.exists():
        out = {"status": "success", "events": [], "count": 0}
        _audit_tool_call("get_recent_audit_events_mcp", {"limit": limit}, out)
        return out

    lines: List[str] = []
    with _audit_lock:
        with _audit_file.open("r", encoding="utf-8") as f:
            lines = f.readlines()
    sliced = lines[-limit:]
    events = []
    for line in sliced:
        try:
            events.append(json.loads(line))
        except Exception:
            continue
    out = {"status": "success", "events": events, "count": len(events)}
    _audit_tool_call("get_recent_audit_events_mcp", {"limit": limit}, {"status": "success", "count": len(events)})
    return out


@app.tool()
def reset_guard_mcp(target: str = "all") -> Dict[str, Any]:
    """手动重置熔断状态。target 可为 all 或具体 key（如 read:2）。"""
    tgt = (target or "all").strip()
    cleared: List[str] = []

    with _circuit_lock:
        if tgt == "all":
            for key, st in _circuit_state.items():
                st["failures"] = 0
                st["open_until"] = 0.0
                cleared.append(key)
        else:
            st = _circuit_state.get(tgt)
            if st is not None:
                st["failures"] = 0
                st["open_until"] = 0.0
                cleared.append(tgt)

    out = {
        "status": "success",
        "target": tgt,
        "cleared": cleared,
        "cleared_count": len(cleared),
    }
    _audit_tool_call("reset_guard_mcp", {"target": target}, out)
    return out


@app.tool()
def mcp_health_report_mcp(recent_limit: int = 20) -> Dict[str, Any]:
    """统一健康报告：串口、设备自检、守护状态、最近审计摘要。"""
    serial_cfg = check_serial_config_mcp()
    self_check = device_self_check_mcp(include_relay_write_check=False, timeout_ms=3000)
    guard = get_guard_status_mcp()
    recent = get_recent_audit_events_mcp(limit=recent_limit)

    guard_circuits = guard.get("circuits", {})
    open_circuits = [k for k, v in guard_circuits.items() if bool(v.get("is_open"))]

    recent_events = recent.get("events", []) if recent.get("status") == "success" else []
    fail_events = []
    for ev in recent_events:
        if not isinstance(ev, dict):
            continue
        st = ev.get("status")
        if st in ("failed", "error", "blocked"):
            fail_events.append(ev)

    if self_check.get("health_status") == "failed" or open_circuits:
        overall = "failed"
    elif self_check.get("health_status") == "degraded":
        overall = "degraded"
    else:
        overall = "ok"

    out = {
        "status": "success",
        "overall_health": overall,
        "serial_status": serial_cfg.get("status"),
        "self_check_health": self_check.get("health_status"),
        "open_circuits": open_circuits,
        "open_circuit_count": len(open_circuits),
        "recent_fail_event_count": len(fail_events),
        "components": {
            "serial_config": serial_cfg,
            "self_check": self_check,
            "guard_status": guard,
            "recent_audit_summary": {
                "limit": recent_limit,
                "count": recent.get("count", 0),
                "recent_fail_events": fail_events[-10:],
            },
        },
    }
    _audit_tool_call("mcp_health_report_mcp", {"recent_limit": recent_limit}, out)
    return out


_skill_engine = SkillEngine(
    device_self_check=device_self_check_mcp,
    health_report=mcp_health_report_mcp,
    recent_audit=get_recent_audit_events_mcp,
    snapshot=read_environment_snapshot_mcp,
    threshold_eval=evaluate_environment_thresholds_mcp,
    safety_control=run_safety_control_mcp,
    relay_all_off=relay_all_off_mcp,
)


@app.tool()
def run_patrol_skill_mcp(
    include_relay_write_check: bool = False,
    recent_limit: int = 20,
) -> Dict[str, Any]:
    """技能层：设备巡检工作流（自检+健康报告+审计摘要）。"""
    out = _skill_engine.run_patrol_skill(
        include_relay_write_check=include_relay_write_check,
        recent_limit=recent_limit,
    )
    _audit_tool_call(
        "run_patrol_skill_mcp",
        {"include_relay_write_check": include_relay_write_check, "recent_limit": recent_limit},
        out,
    )
    return out


@app.tool()
def run_environment_skill_mcp(
    samples: int = 3,
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """技能层：环境评估工作流（快照+阈值分析）。"""
    out = _skill_engine.run_environment_skill(samples=samples, timeout_ms=timeout_ms)
    _audit_tool_call("run_environment_skill_mcp", {"samples": samples, "timeout_ms": timeout_ms}, out)
    return out


@app.tool()
def run_safe_control_skill_mcp(
    strategy: str = "auto",
    require_confirmation: bool = True,
    safety_confirm: bool = False,
    relay_channel: int = 1,
    relay_on_duration_sec: int = 8,
    timeout_ms: int = 5000,
) -> Dict[str, Any]:
    """技能层：安全控制工作流（auto/emergency_stop/pulse）。"""
    out = _skill_engine.run_safe_control_skill(
        strategy=strategy,
        require_confirmation=require_confirmation,
        safety_confirm=safety_confirm,
        relay_channel=relay_channel,
        relay_on_duration_sec=relay_on_duration_sec,
        timeout_ms=timeout_ms,
    )
    _audit_tool_call(
        "run_safe_control_skill_mcp",
        {
            "strategy": strategy,
            "require_confirmation": require_confirmation,
            "safety_confirm": safety_confirm,
            "relay_channel": relay_channel,
            "relay_on_duration_sec": relay_on_duration_sec,
            "timeout_ms": timeout_ms,
        },
        out,
    )
    return out


async def main():
    print("[MCP] server starting", file=sys.stderr, flush=True)
    await app.run_streamable_http_async()


if __name__ == "__main__":
    asyncio.run(main())
