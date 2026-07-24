# M-IR 后端桥接层
#
# 职责：把产品自有的 M-IR 编译器 (mlang.compiler) 与 PC 端 MVM 模拟器
# (mlang.simulator.simulate_subset) 接进运行时。
#
# 链路：LLM 生成 M-IR → 确定性编译成 M-Token 字节码 → PC 端模拟执行。
#
# 后半部分（真实硬件）待有 ESP8266 时，把 simulate 换成 core.execute_m_logic 即可，
# 编译前端完全不变。

import os
import sys
from pathlib import Path
from typing import Dict, Any, Optional

# 本模块自身目录（确保同目录的 pc_resources 可被导入）
_SELF_DIR = Path(__file__).resolve().parent
if str(_SELF_DIR) not in sys.path:
    sys.path.insert(0, str(_SELF_DIR))

# 产品自有的编译器与 MVM 模拟器
from mlang import compiler, simulator  # noqa: E402

# ---------------------------------------------------------------------------
# 路线 A：把 PC 资源"设备化"。
#
# 设计：M-IR 里用标识符槽位（file0/proc0/net0…），真实路径/进程名/地址
# 走旁路声明，字节码与 M-IR 语法都不出现字符串。这样无需改动编译器源码——
# 只在运行时扩展 compiler 的设备表即可，物理硬件与 PC 资源共用同一套 IOW/IOR。
#
# 当前阶段只验证"编译 + 门控 + 模拟"成立，device_id>=200 的 IOW/IOR 不落地真实
# syscall（行动平面的能力模型待确认后再实现）。
# ---------------------------------------------------------------------------

_PC_DEVICE_BASE = {
    "file": 200,   # 文件槽 file0..file9  -> 200..209
    "proc": 210,   # 进程槽 proc0..proc9  -> 210..219
    "net": 220,    # 网络槽 net0..net9    -> 220..229
}
_PC_SLOTS_PER_KIND = 10

PC_DEVICE_IDS: Dict[str, int] = {}
for _kind, _base in _PC_DEVICE_BASE.items():
    for _i in range(_PC_SLOTS_PER_KIND):
        PC_DEVICE_IDS["%s%d" % (_kind, _i)] = _base + _i

# 运行时注入编译器与模拟器共识的设备表。
compiler.DEVICE_IDS.update(PC_DEVICE_IDS)
compiler.WRITABLE_DEVICES |= set(PC_DEVICE_IDS)  # PC 资源槽既可读也可写


import re as _re


def _normalize_task_name(lir_text: str) -> str:
    """把 `task <名字> {` 里的非 ASCII 标识符名字规范化为安全名。

    task 名是纯装饰——不进字节码，只用于可读性。弱模型（尤其中文提问时）
    常给出中文任务名，触发 MIR_PARSE_ERROR。此处在编译前做零语义风险的清洗，
    消掉一整类失败，而不必改动论文编译器的语法。
    """
    m = _re.match(r"^(\s*task\s+)(\S+)(\s*\{)", lir_text)
    if not m:
        return lir_text
    name = m.group(2)
    if _re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        return lir_text  # 已是合法标识符，不动
    safe = _re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not safe or not _re.match(r"[A-Za-z_]", safe):
        safe = "task_" + safe if safe else "auto_task"
    return lir_text[:m.start(2)] + safe + lir_text[m.end(2):]


def compile_lir(lir_text: str) -> Dict[str, Any]:
    """把 M-IR 文本编译成 M-Token 字节码。

    成功返回 {"ok": True, "payload": bytes, "task_name": str, "requirements": [...]}
    失败返回 CompilerError.to_dict()，即 {"ok": False, "error_code", "message", "line"}
    """
    lir_text = _normalize_task_name(lir_text)
    try:
        program, payload = compiler.compile_source(lir_text)
    except compiler.CompilerError as e:
        return e.to_dict()
    except Exception as e:  # 语法/解析类兜底，仍给结构化错误让 LLM 自修
        return {"ok": False, "error_code": "COMPILE_ERROR", "message": str(e), "line": None}
    return {
        "ok": True,
        "payload": payload,
        "task_name": program.task_name,
        "requirements": list(program.requirements),
    }


def execute_lir(
    lir_text: str,
    sensor_values: Optional[Dict[int, int]] = None,
    step_limit: int = 1_000_000,
    max_stack: int = 256,
) -> Dict[str, Any]:
    """M-IR → 字节码 → PC 端模拟执行（无硬件）。返回统一的结构化结果。"""
    compiled = compile_lir(lir_text)
    if not compiled["ok"]:
        return {"status": "compile_error", "lir": lir_text, "error": compiled}

    payload: bytes = compiled["payload"]
    res = simulator.simulate_subset(
        payload,
        sensor_values=sensor_values,
        step_limit=step_limit,
        max_stack=max_stack,
    )

    if res.verify_pass and res.execution_pass:
        status = "success"
    elif not res.verify_pass:
        status = "verify_fault"
    else:
        status = "exec_fault"

    return {
        "status": status,
        "lir": lir_text,
        "task_name": compiled["task_name"],
        "requirements": compiled["requirements"],
        "bytecode_hex": payload.hex(),
        "bytecode_len": len(payload),
        "simulation": {
            "verify_pass": res.verify_pass,
            "execution_pass": res.execution_pass,
            "stage": res.stage,
            "error_code": res.error_code,
            "message": res.message,
            "steps": res.steps,
            "result_top": res.result_top,
            "relay_state": res.relay_state,
        },
        "note": "模拟执行（无 ESP8266 硬件）；relay_state 为虚拟继电器状态。有硬件后改走 core.execute_m_logic。",
    }


# device_id -> 槽位名，用于物化时反查绑定
_ID_TO_SLOT: Dict[int, str] = {v: k for k, v in PC_DEVICE_IDS.items()}


def execute_lir_with_side_effects(
    lir_text: str,
    resource_bindings: Dict[str, Dict[str, Any]],
    sensor_values: Optional[Dict[int, int]] = None,
) -> Dict[str, Any]:
    """行动平面：M-IR 编译+模拟演练全通过后，物化文件槽的真实写入。

    两阶段：先复用 execute_lir 做零副作用演练；仅当 status==success 才调用
    pc_resources 物化。演练失败则原样返回，绝不产生任何真实副作用。

    resource_bindings: 受信任的旁路绑定，如 {"file0": {"path": "temp.log", "content": "25"}}
                       路径相对沙箱根解释，内容来自这里而非 LLM 的 M-IR。
    """
    import pc_resources

    dry = execute_lir(lir_text, sensor_values=sensor_values)
    if dry["status"] != "success":
        dry["materialized"] = {"ok": False, "written": [], "error": "dry-run not success; no side effects"}
        return dry

    mat = pc_resources.materialize_files(
        device_state=dry["simulation"]["relay_state"],
        resource_bindings=resource_bindings,
        id_to_slot=_ID_TO_SLOT,
        sensor_values=sensor_values,
    )
    dry["materialized"] = mat
    if not mat["ok"]:
        dry["status"] = "side_effect_error"
    return dry


def load_trusted_bindings() -> Dict[str, Dict[str, Any]]:
    """从服务端受信配置(MCP_LIR_BINDINGS 指向的 JSON)加载槽位->资源绑定。

    这是"参数隔离"的关键：绑定来自服务端配置，不来自 LLM。LLM 生成的 M-IR
    只能引用槽位名(file0…)，无权指定真实路径。配置缺失时返回空表(无可用槽位)。
    """
    import json
    cfg = os.getenv("MCP_LIR_BINDINGS", "").strip()
    if not cfg:
        return {}
    p = Path(cfg)
    if not p.is_file():
        return {}
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (ValueError, OSError):
        return {}
    return data if isinstance(data, dict) else {}
