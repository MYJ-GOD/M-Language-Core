# PC 资源行动平面（最小真实执行器）
#
# 职责：把 LIR 编译并模拟通过后的"文件槽"终态，物化为真实的本机文件写入。
# 这是从"零副作用验证"跨到"真实 syscall"的第一步，因此安全边界收得很紧：
#
#   1. 只支持文件"写"，不碰进程/网络（proc/net 槽被激活则拒绝整次物化）。
#   2. 真实路径与内容来自受信任的旁路绑定(resource_bindings)，不来自 LLM 的 LIR。
#      LLM 只能引用槽位名(file0…)，无权指定路径。
#   3. 所有路径相对沙箱根目录(MCP_LIR_FILE_ROOT，默认 data/lir_sandbox)解释，
#      resolve 后必须仍在根内，杜绝 ../ 目录穿越。
#   4. 两阶段：调用方先跑纯模拟演练，仅当 verify+execution 全通过才调用本模块物化。

import os
import socket
import subprocess
from pathlib import Path
from typing import Dict, Any, Optional

# 与 lir_backend.PC_DEVICE_IDS 一致的地址区间
_FILE_ID_LO, _FILE_ID_HI = 200, 209
_PROC_ID_LO, _PROC_ID_HI = 210, 219
_NET_ID_LO, _NET_ID_HI = 220, 229

_FILE_ROOT = Path(os.getenv("MCP_LIR_FILE_ROOT", "data/lir_sandbox")).resolve()


def _resolve_in_sandbox(rel_path: str) -> Path:
    """把绑定里的路径拼到沙箱根下并校验不越界；越界抛 ValueError。"""
    candidate = (_FILE_ROOT / rel_path).resolve()
    if candidate != _FILE_ROOT and _FILE_ROOT not in candidate.parents:
        raise ValueError(
            "path %r escapes sandbox root %s" % (rel_path, _FILE_ROOT)
        )
    return candidate


def materialize_files(
    device_state: Dict[int, int],
    resource_bindings: Dict[str, Dict[str, Any]],
    id_to_slot: Dict[int, str],
    sensor_values: Optional[Dict[int, int]] = None,
) -> Dict[str, Any]:
    """根据模拟终态物化文件写入。

    device_state:      模拟后的设备终态 {device_id: value}
    resource_bindings: 槽位 -> {"path": <沙箱内相对路径>, "content": <字符串>, "source_device": <device_id>}
                       - content: 静态内容（向后兼容）
                       - source_device: 从 device_state 或 sensor_values 动态获取内容（优先级高于 content）
    id_to_slot:        device_id -> 槽位名(用于反查)
    sensor_values:     传感器值 {device_id: value}，用于 source_device 查找

    返回 {"ok": bool, "written": [...], "error": str|None}
    """
    written = []
    try:
        for dev, val in device_state.items():
            if not (_FILE_ID_LO <= dev <= _FILE_ID_HI):
                continue
            if not val:  # 终态未激活的文件槽不写
                continue
            slot = id_to_slot.get(dev)
            binding = resource_bindings.get(slot) if slot else None
            if not binding:
                return {"ok": False, "written": written,
                        "error": "file slot %r activated but has no trusted binding" % slot}
            target = _resolve_in_sandbox(binding["path"])
            target.parent.mkdir(parents=True, exist_ok=True)

            # 动态内容：从 source_device 获取设备值
            source_device = binding.get("source_device")
            if source_device is not None:
                # 优先从 device_state 获取，其次从 sensor_values 获取
                if source_device in device_state:
                    content = str(device_state[source_device])
                elif sensor_values and source_device in sensor_values:
                    content = str(sensor_values[source_device])
                else:
                    return {"ok": False, "written": written,
                            "error": "source_device %d not found in device_state or sensor_values" % source_device}
            else:
                # 静态内容（向后兼容）
                content = str(binding.get("content", ""))

            target.write_text(content, encoding="utf-8")
            written.append({"slot": slot, "device_id": dev, "path": str(target), "bytes": len(content)})
    except (ValueError, OSError) as e:
        return {"ok": False, "written": written, "error": str(e)}

    return {"ok": True, "written": written, "error": None}


# ---------------------------------------------------------------------------
# 进程槽：proc0..proc9
# ---------------------------------------------------------------------------

# 进程执行结果缓存 {slot: {"returncode": int, "stdout": str, "stderr": str}}
_PROC_RESULTS: Dict[str, Dict[str, Any]] = {}


def materialize_processes(
    device_state: Dict[int, int],
    resource_bindings: Dict[str, Dict[str, Any]],
    id_to_slot: Dict[int, str],
) -> Dict[str, Any]:
    """根据模拟终态执行进程操作。

    device_state:      模拟后的设备终态 {device_id: value}
    resource_bindings: 槽位 -> {"command": <命令>, "args": <参数列表>, "cwd": <工作目录>}
                       - command: 要执行的命令（必填）
                       - args: 命令参数列表（可选）
                       - cwd: 工作目录（可选，默认为沙箱根目录）
    id_to_slot:        device_id -> 槽位名(用于反查)

    返回 {"ok": bool, "results": [...], "error": str|None}
    """
    results = []
    try:
        for dev, val in device_state.items():
            if not (_PROC_ID_LO <= dev <= _PROC_ID_HI):
                continue
            if not val:  # 终态未激活的进程槽不执行
                continue

            slot = id_to_slot.get(dev)
            binding = resource_bindings.get(slot) if slot else None
            if not binding:
                return {"ok": False, "results": results,
                        "error": "proc slot %r activated but has no trusted binding" % slot}

            command = binding.get("command")
            if not command:
                return {"ok": False, "results": results,
                        "error": "proc slot %r binding missing 'command'" % slot}

            args = binding.get("args", [])
            cwd = binding.get("cwd")
            if cwd:
                cwd = str(_resolve_in_sandbox(cwd))

            # 执行进程
            try:
                proc = subprocess.run(
                    [command] + args,
                    capture_output=True,
                    text=True,
                    cwd=cwd or str(_FILE_ROOT),
                    timeout=30,  # 30 秒超时
                )
                result = {
                    "slot": slot,
                    "device_id": dev,
                    "returncode": proc.returncode,
                    "stdout": proc.stdout[:1024],  # 限制输出长度
                    "stderr": proc.stderr[:1024],
                }
                _PROC_RESULTS[slot] = result
                results.append(result)
            except subprocess.TimeoutExpired:
                return {"ok": False, "results": results,
                        "error": "proc slot %r execution timed out (30s)" % slot}
            except OSError as e:
                return {"ok": False, "results": results,
                        "error": "proc slot %r execution failed: %s" % (slot, str(e))}

    except (ValueError, OSError) as e:
        return {"ok": False, "results": results, "error": str(e)}

    return {"ok": True, "results": results, "error": None}


def get_proc_result(slot: str) -> Optional[Dict[str, Any]]:
    """获取进程槽的最近一次执行结果（用于 IOR 读取）。"""
    return _PROC_RESULTS.get(slot)


# ---------------------------------------------------------------------------
# 网络槽：net0..net9
# ---------------------------------------------------------------------------

# 网络执行结果缓存 {slot: {"sent": int, "received": str, "error": str|None}}
_NET_RESULTS: Dict[str, Dict[str, Any]] = {}


def materialize_network(
    device_state: Dict[int, int],
    resource_bindings: Dict[str, Dict[str, Any]],
    id_to_slot: Dict[int, str],
) -> Dict[str, Any]:
    """根据模拟终态执行网络操作（TCP 收发）。

    device_state:      模拟后的设备终态 {device_id: value}
    resource_bindings: 槽位 -> {"host": <主机>, "port": <端口>, "send_data": <发送数据>, "recv_size": <接收大小>}
                       - host: 目标主机（必填）
                       - port: 目标端口（必填）
                       - send_data: 要发送的数据（可选，默认为空）
                       - recv_size: 接收缓冲区大小（可选，默认 4096）
    id_to_slot:        device_id -> 槽位名(用于反查)

    返回 {"ok": bool, "results": [...], "error": str|None}
    """
    results = []
    try:
        for dev, val in device_state.items():
            if not (_NET_ID_LO <= dev <= _NET_ID_HI):
                continue
            if not val:  # 终态未激活的网络槽不执行
                continue

            slot = id_to_slot.get(dev)
            binding = resource_bindings.get(slot) if slot else None
            if not binding:
                return {"ok": False, "results": results,
                        "error": "net slot %r activated but has no trusted binding" % slot}

            host = binding.get("host")
            port = binding.get("port")
            if not host or not port:
                return {"ok": False, "results": results,
                        "error": "net slot %r binding missing 'host' or 'port'" % slot}

            send_data = binding.get("send_data", "")
            recv_size = binding.get("recv_size", 4096)

            # 执行网络操作
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                    sock.settimeout(10)  # 10 秒超时
                    sock.connect((host, int(port)))

                    # 发送数据
                    sent_bytes = 0
                    if send_data:
                        sent_bytes = sock.send(send_data.encode("utf-8"))

                    # 接收数据
                    received = ""
                    try:
                        data = sock.recv(int(recv_size))
                        received = data.decode("utf-8", errors="replace")
                    except socket.timeout:
                        pass  # 超时不报错，返回已接收的数据

                    result = {
                        "slot": slot,
                        "device_id": dev,
                        "host": host,
                        "port": port,
                        "sent": sent_bytes,
                        "received": received[:1024],  # 限制输出长度
                        "error": None,
                    }
                    _NET_RESULTS[slot] = result
                    results.append(result)

            except socket.timeout:
                return {"ok": False, "results": results,
                        "error": "net slot %r connection timed out (10s)" % slot}
            except OSError as e:
                return {"ok": False, "results": results,
                        "error": "net slot %r network failed: %s" % (slot, str(e))}

    except (ValueError, OSError) as e:
        return {"ok": False, "results": results, "error": str(e)}

    return {"ok": True, "results": results, "error": None}


def get_net_result(slot: str) -> Optional[Dict[str, Any]]:
    """获取网络槽的最近一次执行结果（用于 IOR 读取）。"""
    return _NET_RESULTS.get(slot)
