# M-Language MCP Server
# FastMCP 工具定义层 - 只负责暴露接口，不含业务逻辑

import asyncio
import os
import sys
from typing import Dict, Any, List

from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings
from core import (
    get_hardware_topology,
    execute_m_logic,
    read_vm_state,
    detect_serial_ports
)

_host = os.getenv("MCP_HTTP_HOST", "127.0.0.1")
_port = int(os.getenv("MCP_HTTP_PORT", "9001"))

# Disable DNS rebinding protection for container-to-container access.
app = FastMCP(
    "m-language-mcp",
    host=_host,
    port=_port,
    transport_security=TransportSecuritySettings(enable_dns_rebinding_protection=False),
)


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
    return get_hardware_topology()


@app.tool()
def read_water_level_mcp() -> Dict[str, Any]:
    """直接读取水位传感器，自动处理串口配置。"""
    import os
    import serial.tools.list_ports

    # 1. 检查串口是否已配置
    port = os.getenv("MCP_SERIAL_PORT", "")

    if not port:
        # 2. 自动检测串口
        try:
            ports = list(serial.tools.list_ports.comports())
        except Exception:
            ports = []

        if not ports:
            return {
                "status": "error",
                "error": "未检测到任何串口设备",
                "hint": "请确保 ESP8266 已通过 USB 连接到电脑",
                "solution": "1. 检查 USB 连接\n2. 安装 CH340 驱动\n3. 重试"
            }

        # 选择第一个 USB 串口
        port = ports[0].device

        # 临时设置（仅当前进程）
        os.environ["MCP_SERIAL_PORT"] = port

    # 3. 执行读取水位指令
    m_tokens = [80, 1, 71, 1, 82]
    result = execute_m_logic(m_tokens, timeout_ms=5000)

    # 4. 添加串口信息到返回结果
    result["port_used"] = port
    result["status"] = "success" if result.get("completed") else "failed"

    if not result.get("completed"):
        result["hint"] = f"使用串口 {port} 执行失败，请检查：\n1. 设备是否正确连接\n2. 固件是否已上传\n3. 尝试重新插拔 USB"

    return result


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
    return execute_m_logic(m_tokens, timeout_ms)


@app.tool()
def check_serial_config_mcp() -> Dict[str, Any]:
    """检查串口配置。"""
    import os
    port = os.getenv("MCP_SERIAL_PORT", "")
    baud = os.getenv("MCP_BAUD", "115200")

    if not port:
        return {
            "status": "not_set",
            "error": "MCP_SERIAL_PORT environment variable not set",
            "solution": "Set before running MCP server:",
            "windows": "$env:MCP_SERIAL_PORT='COM3'",
            "linux_mac": "export MCP_SERIAL_PORT='/dev/ttyUSB0'",
            "example_check": "Get-ChildItem Env:MCP_SERIAL_PORT"
        }

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

    return {
        "status": "ok" if port_exists else "port_not_found",
        "MCP_SERIAL_PORT": port,
        "MCP_BAUD": baud,
        "port_exists": port_exists,
        "hint": f"Ensure ESP8266 is connected to {port}" if port_exists else f"Port {port} not found. Check connection."
    }


@app.tool()
def detect_and_connect_mcp() -> Dict[str, Any]:
    """自动检测并连接 ESP8266 串口。"""
    import os

    # 调用 core 中的串口检测函数
    detected = detect_serial_ports()

    if detected["status"] == "error":
        return {
            "status": "error",
            "error": detected.get("error", "Unknown error"),
            "solution": "请检查 pyserial 是否已安装: pip install pyserial",
            "windows": "确保 USB 转 TTL 驱动已安装",
            "linux_mac": "检查用户是否有串口权限: sudo usermod -aG dialout $USER"
        }

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

    return {
        "status": detected["status"],
        "total_ports": detected.get("total_ports", 0),
        "all_ports": detected.get("all_ports", []),
        "esp8266_candidates": detected.get("esp8266_candidates", []),
        "suggested_port": detected.get("suggested_port"),
        "quick_action": quick_action,
        "hint": detected.get("hint", "")
    }


@app.tool()
def read_vm_state_mcp(
    stack_depth: int = 8,
) -> Dict[str, Any]:
    """读取 VM 运行时状态。"""
    return read_vm_state(stack_depth)


async def main():
    print("[MCP] server starting", file=sys.stderr, flush=True)
    await app.run_streamable_http_async()


if __name__ == "__main__":
    asyncio.run(main())
