import os
import time
from typing import List, Dict, Any, Tuple

try:
    import serial  # type: ignore
except Exception:
    serial = None


def get_hardware_topology() -> Dict[str, Any]:
    """获取设备清单"""
    return {
        "devices": [
            {"id": 1, "type": "SENSOR", "name": "Water_Level", "unit": "raw_0_1024"},
            {"id": 2, "type": "SENSOR", "name": "DHT11_Temp", "unit": "Celsius"},
            {"id": 5, "type": "ACTUATOR", "name": "Relay_1", "desc": "Controls water pump"},
            {"id": 6, "type": "ACTUATOR", "name": "Onboard_LED", "desc": "Indicator"}
        ],
        "auth_required": [70, 71]
    }


def _encode_uvarint(value: int) -> bytes:
    if value < 0:
        raise ValueError("uvarint cannot be negative")
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


def _encode_zigzag64(n: int) -> int:
    return (n << 1) ^ (n >> 63)


def _encode_token(val: int) -> bytes:
    if val < 0:
        return _encode_uvarint(_encode_zigzag64(val))
    return _encode_uvarint(val)


def _encode_varint(tokens: List[int]) -> bytes:
    buf = bytearray()
    for t in tokens:
        buf.extend(_encode_token(int(t)))
    return bytes(buf)


def _decode_uvarint(data: bytes, offset: int) -> Tuple[int, int]:
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


def _parse_response(payload: bytes) -> Dict[str, Any]:
    # [result/fault varint][steps/pc varint][flag]
    if len(payload) < 1:
        return {"error": "empty response"}
    flag = payload[-1]
    try:
        first, off = _decode_uvarint(payload, 0)
        second, off2 = _decode_uvarint(payload, off)
    except Exception as exc:
        return {"error": f"bad response varint: {exc}"}

    if flag == 0x01:
        return {"result": first, "steps": second, "completed": True}
    return {"fault_code": first, "pc": second, "completed": False}


def _serial_send(bytecode: bytes, timeout_ms: int) -> Dict[str, Any]:
    port = os.getenv("MCP_SERIAL_PORT", "")
    baud = int(os.getenv("MCP_BAUD", "115200"))
    if not port:
        return {"error": "MCP_SERIAL_PORT not set"}
    if serial is None:
        return {"error": "pyserial not installed"}

    ser = serial.Serial(port=port, baudrate=baud, timeout=timeout_ms / 1000.0)
    try:
        ser.write(len(bytecode).to_bytes(4, "little"))
        ser.write(bytecode)
        resp_len_raw = ser.read(4)
        if len(resp_len_raw) < 4:
            return {"error": "timeout reading response length"}
        resp_len = int.from_bytes(resp_len_raw, "little")
        resp = ser.read(resp_len)
        if len(resp) < resp_len:
            return {"error": "timeout reading response payload"}
        return _parse_response(resp)
    finally:
        ser.close()


def _uses_io(tokens: List[int]) -> bool:
    io_ops = {70, 71}
    return any(int(t) in io_ops for t in tokens)


def execute_m_logic(m_tokens: List[int], description: str = "", timeout_ms: int = 5000) -> Dict[str, Any]:
    """下发 M-Token 执行"""
    if not isinstance(m_tokens, list) or not m_tokens:
        return {"error": "m_tokens must be a non-empty list"}

    # Basic validation: ensure integers
    for t in m_tokens:
        if not isinstance(t, int):
            return {"error": "m_tokens must be integers"}

    # Optional warning for IO usage without GTWAY
    if _uses_io(m_tokens) and 80 not in m_tokens:
        return {"error": "IO requires GTWAY capability (opcode 80) in token stream"}

    bytecode = _encode_varint(m_tokens)
    return _serial_send(bytecode, timeout_ms)


def read_vm_state(stack_depth: int = 8) -> Dict[str, Any]:
    """读取 VM 状态（占位，设备侧未实现时返回模拟值）"""
    return {
        "completed": True,
        "fault": "NONE",
        "fault_code": 0,
        "steps": 15,
        "sp": 0,
        "stack": [42][:max(1, int(stack_depth))],
        "result": 42
    }
