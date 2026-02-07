# M-Language Core Module
# 核心实现：编码/解码、串口通信、协议处理

import os
import time
from typing import List, Dict, Any, Tuple

try:
    import serial  # type: ignore
except Exception:
    serial = None


# =============================================================================
# 设备定义
# =============================================================================

def get_hardware_topology() -> Dict[str, Any]:
    """获取设备清单"""
    return {
        "controller": {
            "name": "NodeMCU (ESP8266)",
            "analog_in": "A0",
            "digital_pins": {
                "D1": "GPIO5",
                "D2": "GPIO4",
                "D4": "GPIO2"
            }
        },
        "devices": [
            {
                "id": 1,
                "type": "SENSOR",
                "name": "Water_Level",
                "signal": "AO",
                "pin": "A0",
                "unit": "raw_0_1024"
            },
            {
                "id": 2,
                "type": "SENSOR",
                "name": "DHT11_Temp",
                "pin": "D4",
                "unit": "Celsius"
            },
            {
                "id": 3,
                "type": "SENSOR",
                "name": "DHT11_Humidity",
                "pin": "D4",
                "unit": "Percent"
            },
            {
                "id": 5,
                "type": "ACTUATOR",
                "name": "Relay_1",
                "pin": "D1",
                "desc": "Relay channel 1"
            },
            {
                "id": 6,
                "type": "ACTUATOR",
                "name": "Relay_2",
                "pin": "D2",
                "desc": "Relay channel 2"
            }
        ],
        "auth_required": [70, 71]
    }


# =============================================================================
# M-Token 编码/解码
# =============================================================================

def _encode_uvarint(value: int) -> bytes:
    """Encode unsigned varint."""
    if value < 0:
        raise ValueError("uvarint cannot be negative")
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


def _encode_zigzag64(n: int) -> int:
    """Encode signed int64 to unsigned zigzag."""
    return (n << 1) ^ (n >> 63)


def _encode_token(val: int) -> bytes:
    """Encode a single token (opcode or operand)."""
    if val < 0:
        return _encode_uvarint(_encode_zigzag64(val))
    return _encode_uvarint(val)


def encode_varint(tokens: List[int]) -> bytes:
    """Encode M-Token list to bytecode."""
    buf = bytearray()
    for t in tokens:
        buf.extend(_encode_token(int(t)))
    return bytes(buf)


def _decode_uvarint(data: bytes, offset: int) -> Tuple[int, int]:
    """Decode unsigned varint, return (value, next_offset)."""
    shift = 0
    result = 0
    i = offset
    while i < len(data):
        b = data[i]
        result |= (b & 0x7F) << shift
        i += 1
        if (b & 0x80) == 0:
            return result, i
        shift += 7
        if shift > 63:
            break
    raise ValueError("bad varint")


def decode_varint(data: bytes) -> List[int]:
    """Decode bytecode to M-Token list."""
    tokens = []
    offset = 0
    while offset < len(data):
        value, next_offset = _decode_uvarint(data, offset)
        tokens.append(value)
        offset = next_offset
    return tokens


def parse_response(payload: bytes) -> Dict[str, Any]:
    """Parse VM response: [result/fault varint][steps varint][flag]."""
    if len(payload) < 1:
        return {"error": "empty response"}

    flag = payload[-1]

    try:
        first, off = _decode_uvarint(payload, 0)
        second, off2 = _decode_uvarint(payload, off)
    except Exception as exc:
        return {"error": f"bad response varint: {exc}"}

    if flag == 0x01:
        return {
            "result": first,
            "steps": second,
            "completed": True,
            "fault_code": 0,
            "fault": "NONE"
        }
    else:
        fault_names = {
            1: "STACK_OVERFLOW",
            2: "STACK_UNDERFLOW",
            3: "RET_STACK_OVERFLOW",
            4: "RET_STACK_UNDERFLOW",
            5: "LOCALS_OOB",
            6: "GLOBALS_OOB",
            7: "PC_OOB",
            8: "DIV_BY_ZERO",
            9: "MOD_BY_ZERO",
            10: "UNKNOWN_OP",
            11: "STEP_LIMIT",
            12: "GAS_EXHAUSTED",
            13: "BAD_ENCODING",
            14: "UNAUTHORIZED",
            15: "TYPE_MISMATCH",
            16: "INDEX_OOB",
            17: "BAD_ARG",
            18: "OOM",
            19: "ASSERT_FAILED",
            20: "BREAKPOINT",
            21: "DEBUG_STEP",
            22: "CALL_DEPTH_LIMIT",
            99: "GAS_LIMIT"
        }
        return {
            "result": None,
            "fault_code": first,
            "pc": second,
            "completed": False,
            "fault": fault_names.get(first, f"UNKNOWN_{first}")
        }


# =============================================================================
# 串口通信
# =============================================================================

def _serial_send(bytecode: bytes, timeout_ms: int) -> Dict[str, Any]:
    """Send bytecode to ESP8266 and get response."""
    port = os.getenv("MCP_SERIAL_PORT", "")
    baud = int(os.getenv("MCP_BAUD", "115200"))

    if not port:
        return {"error": "MCP_SERIAL_PORT not set", "code": "CONFIG_MISSING"}
    if serial is None:
        return {"error": "pyserial not installed", "code": "MISSING_DEP"}

    try:
        ser = serial.Serial(port=port, baudrate=baud, timeout=timeout_ms / 1000.0)
    except Exception as exc:
        return {"error": f"cannot open port {port}: {exc}", "code": "PORT_ERROR"}

    try:
        time.sleep(0.05)  # Settle time
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        # Send length prefix + bytecode
        ser.write(len(bytecode).to_bytes(4, "little"))
        ser.write(bytecode)

        # Read response
        resp_len_raw = ser.read(4)
        if len(resp_len_raw) < 4:
            return {"error": "timeout reading response length", "code": "TIMEOUT_LEN"}

        resp_len = int.from_bytes(resp_len_raw, "little")
        resp = ser.read(resp_len)
        if len(resp) < resp_len:
            return {"error": "timeout reading response payload", "code": "TIMEOUT_RESP"}

        return parse_response(resp)

    finally:
        ser.close()


# =============================================================================
# M-Token 执行
# =============================================================================

def uses_io(tokens: List[int]) -> bool:
    """Check if token list contains IO operations."""
    io_ops = {70, 71}  # IOW, IOR
    return any(int(t) in io_ops for t in tokens)


def needs_capability(tokens: List[int], cap_id: int) -> bool:
    """Check if token list contains GTWAY with specific capability."""
    for i, t in enumerate(tokens):
        if t == 80 and i + 1 < len(tokens):
            if tokens[i + 1] == cap_id:
                return True
    return False


def execute_m_logic(m_tokens: List[Any], timeout_ms: int = 5000) -> Dict[str, Any]:
    """
    Execute M-Token bytecode on ESP8266.

    Args:
        m_tokens: List of M-Token opcodes/operands (integers only)
        timeout_ms: Timeout in milliseconds

    Returns:
        Dict with result/fault info
    """
    # Validate input
    if not isinstance(m_tokens, list) or not m_tokens:
        return {"error": "m_tokens must be a non-empty list", "code": "INVALID_INPUT"}

    # Check for string arguments (common AI mistake)
    for i, t in enumerate(m_tokens):
        if isinstance(t, str):
            return {
                "error": "M-Tokens must be integers only, not strings",
                "code": "INVALID_TYPE",
                "hint": f"Remove string argument: {t!r}",
                "example": "For water level: [80, 1, 71, 1, 82]",
                "m_tokens_received": m_tokens
            }

    for t in m_tokens:
        if not isinstance(t, int):
            return {"error": f"m_tokens must be integers, got {type(t).__name__}", "code": "INVALID_TYPE"}
        if t < 0 or t > 255:
            return {"error": f"token {t} out of range (0-255)", "code": "TOKEN_RANGE"}
        if t >= 75 and t <= 79:
            return {
                "error": f"Token {t} is reserved/undefined in Core指令集 (0-99)",
                "code": "BAD_OPCODE",
                "hint": "Core指令集: 10-18控制流, 30-33变量, 40-44比较, 50-58算术, 60-63数组, 64-66栈操作, 70-71硬件IO, 80-83系统",
                "m_tokens_received": m_tokens
            }

    # Check IO authorization
    if uses_io(m_tokens):
        # Find which device is being accessed
        for i, t in enumerate(m_tokens):
            if t in (70, 71) and i + 1 < len(m_tokens):
                device_id = m_tokens[i + 1]
                if not needs_capability(m_tokens, device_id):
                    return {
                        "error": f"IO operation on device {device_id} requires GTWAY capability",
                        "code": "UNAUTHORIZED_IO",
                        "device_id": device_id,
                        "hint": "Add GTWAY(cap_id) before IOR/IOW. Example: [80, 1, 71, 1, 82]"
                    }

    bytecode = encode_varint(m_tokens)
    return _serial_send(bytecode, timeout_ms)


# =============================================================================
# VM 状态
# =============================================================================

def read_vm_state(stack_depth: int = 8) -> Dict[str, Any]:
    """
    Read VM runtime state.

    Note: ESP8266 firmware may not implement this yet.
    Returns simulated data if firmware doesn't respond.
    """
    return {
        "completed": True,
        "fault": "NONE",
        "fault_code": 0,
        "steps": 0,
        "sp": 0,
        "stack": [0] * max(1, int(stack_depth)),
        "result": 0,
        "note": "VM state read not yet implemented in firmware"
    }


# =============================================================================
# 串口检测
# =============================================================================

def detect_serial_ports() -> Dict[str, Any]:
    """
    自动检测系统可用串口，识别可能的 ESP8266 设备。

    返回:
        status: "detected" | "none_found" | "error"
        all_ports: 所有可用串口列表
        esp8266_candidates: 可能是 ESP8266 的串口
        suggested_port: 建议使用的串口
        hint: 使用提示
        error: 错误信息（如果有）
    """
    try:
        import serial.tools.list_ports
    except ImportError:
        return {
            "status": "error",
            "error": "pyserial not installed",
            "solution": "Run: pip install pyserial"
        }

    ports = serial.tools.list_ports.comports()

    if not ports:
        return {
            "status": "none_found",
            "total_ports": 0,
            "all_ports": [],
            "esp8266_candidates": [],
            "suggested_port": None,
            "hint": "未检测到任何串口设备。请检查：\n1. ESP8266 是否已通过 USB 连接到电脑\n2. USB 线是否支持数据传输（有些充电线只供电不传数据）\n3. 设备管理器中是否识别到新的 COM 端口"
        }

    # 收集所有串口信息
    all_ports = []
    for port in ports:
        port_info = {
            "device": port.device,
            "description": port.description,
            "hwid": port.hwid,
            "manufacturer": port.manufacturer,
            "product": port.product,
            "vid": port.vid,
            "pid": port.pid,
            "serial_number": port.serial_number
        }
        all_ports.append(port_info)

    # ESP8266 设备识别关键词
    esp8266_keywords = [
        "usb", "usb serial", "uart",
        "ch340", "ch341",  # 常见 USB-UART 芯片
        "cp210", "cp2102", "cp210x",  # Silicon Labs
        "ftdi", "ft232",  # FTDI
        "pl2303",  # Prolific
        "arduino", "nodemcu", "esp8266",
        "serial port", "com port"
    ]

    # 识别可能是 ESP8266 的串口
    esp8266_candidates = []
    for port in all_ports:
        desc_lower = (port.get("description") or "").lower()
        manuf_lower = (port.get("manufacturer") or "").lower()
        hwid_lower = (port.get("hwid") or "").lower()

        is_candidate = any(
            keyword in desc_lower or keyword in manuf_lower or keyword in hwid_lower
            for keyword in esp8266_keywords
        )

        # 过滤掉非串口设备（如蓝牙串口）
        if is_candidate:
            # 排除纯蓝牙串口
            if "bluetooth" not in desc_lower and "bluetooth" not in manuf_lower:
                esp8266_candidates.append(port)

    # 建议使用的串口：优先选择 ESP8266 候选串口，否则选择第一个
    if esp8266_candidates:
        suggested_port = esp8266_candidates[0]
    else:
        suggested_port = all_ports[0] if all_ports else None

    # 生成提示信息
    port_count = len(all_ports)
    if port_count == 1:
        hint = f"检测到 1 个串口设备: {suggested_port['device']}"
    else:
        hint = f"检测到 {port_count} 个串口设备。"

    if esp8266_candidates:
        if len(esp8266_candidates) == 1:
            hint += f"\n建议使用: {esp8266_candidates[0]['device']} ({esp8266_candidates[0]['description']})"
        else:
            hint += f"\n可能是 ESP8266 的串口: {[p['device'] for p in esp8266_candidates]}"
    else:
        if suggested_port:
            hint += f"\n使用建议: {suggested_port['device']} ({suggested_port['description']})"

    return {
        "status": "detected",
        "total_ports": port_count,
        "all_ports": all_ports,
        "esp8266_candidates": esp8266_candidates,
        "suggested_port": suggested_port,
        "hint": hint
    }
