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
from pathlib import Path
from typing import Dict, Any

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
) -> Dict[str, Any]:
    """根据模拟终态物化文件写入。

    device_state:      模拟后的设备终态 {device_id: value}
    resource_bindings: 槽位 -> {"path": <沙箱内相对路径>, "content": <字符串>}
    id_to_slot:        device_id -> 槽位名(用于反查)

    返回 {"ok": bool, "written": [...], "error": str|None}
    """
    # 安全闸门：进程/网络槽被激活一律拒绝（本版未实现，不允许静默跳过）
    for dev, val in device_state.items():
        if val and (_PROC_ID_LO <= dev <= _NET_ID_HI):
            return {
                "ok": False,
                "written": [],
                "error": "proc/net slot %d activated but not supported in this executor; refusing to materialize" % dev,
            }

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
            content = str(binding.get("content", ""))
            target.write_text(content, encoding="utf-8")
            written.append({"slot": slot, "device_id": dev, "path": str(target), "bytes": len(content)})
    except (ValueError, OSError) as e:
        return {"ok": False, "written": written, "error": str(e)}

    return {"ok": True, "written": written, "error": None}
