# M-IR 后端回归测试
#
# 固化"接断点 + 路线A设备化 + 文件槽真实执行"三步的已验证行为。
# 运行：cd tests && python -m pytest test_lir_backend.py -v
#
# 说明：文件写入测试用 tmp_path 作沙箱根，通过 monkeypatch 覆写 pc_resources 的
# _FILE_ROOT，避免污染真实目录、也不依赖环境变量顺序。

import sys
from pathlib import Path

# 添加 python/MCP 到路径，以便导入 lir_backend 等模块
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python" / "MCP"))

import pytest

import lir_backend
from lir_backend import execute_lir, execute_lir_with_side_effects, compile_lir


# ---------------------------------------------------------------------------
# 一、编译 + 模拟（零副作用）
# ---------------------------------------------------------------------------

def test_compile_basic_relay():
    r = compile_lir("task t {\n  require cap(relay1)\n  set relay1 = 1\n  halt\n}")
    assert r["ok"] is True
    assert r["payload"].hex() == "50051e02460552"  # GTWAY5 LIT1 IOW5 HALT


def test_relay_on_success():
    r = execute_lir("task t {\n  require cap(relay1)\n  set relay1 = 1\n  halt\n}")
    assert r["status"] == "success"
    assert r["simulation"]["relay_state"][5] == 1


def test_capability_gate_blocks_undeclared_device():
    # 用了 relay1 却没 require cap(relay1) -> 编译期拦截
    r = execute_lir("task t {\n  set relay1 = 1\n  halt\n}")
    assert r["status"] == "compile_error"
    assert r["error"]["error_code"] == "INVALID_CAPABILITY"
    assert r["error"]["line"] == 2


def test_invalid_set_value():
    r = execute_lir("task t {\n  require cap(relay1)\n  set relay1 = 5\n  halt\n}")
    assert r["status"] == "compile_error"
    assert r["error"]["error_code"] == "INVALID_ARGUMENT"


def test_write_readonly_device():
    r = execute_lir("task t {\n  require cap(temperature_sensor)\n  set temperature_sensor = 1\n  halt\n}")
    assert r["status"] == "compile_error"
    assert r["error"]["error_code"] == "INVALID_SET_TARGET"


def test_syntax_error_is_structured():
    r = execute_lir("this is not valid lir")
    assert r["status"] == "compile_error"
    assert "error_code" in r["error"] and r["error"]["line"] == 1


def test_retry_readback_early_exit_vs_exhaust():
    lir = (
        "task cool {\n"
        "  require cap(relay1)\n"
        "  require cap(temperature_sensor)\n"
        "  set relay1 = 1\n"
        "  retry 5 times {\n"
        "    wait 1000ms\n"
        "    readback temperature_sensor expect 25\n"
        "  }\n"
        "  halt\n}"
    )
    hit = execute_lir(lir, sensor_values={2: 25})
    miss = execute_lir(lir, sensor_values={2: 20})
    assert hit["status"] == "success" and miss["status"] == "success"
    # 命中应提前退出，步数明显少于跑满重试
    assert hit["simulation"]["steps"] < miss["simulation"]["steps"]


# ---------------------------------------------------------------------------
# 二、路线 A：PC 资源设备化（编译 + 门控）
# ---------------------------------------------------------------------------

def test_pc_device_ids_registered():
    assert lir_backend.PC_DEVICE_IDS["file0"] == 200
    assert lir_backend.PC_DEVICE_IDS["proc0"] == 210
    assert lir_backend.PC_DEVICE_IDS["net0"] == 220


def test_hardware_and_pc_share_same_instructions():
    # 温度传感器(硬件) + 文件槽(PC) 同一段 LIR，走同一套 IOW/IOR
    r = execute_lir(
        "task log {\n  require cap(temperature_sensor)\n  require cap(file0)\n"
        "  read temperature_sensor\n  set file0 = 1\n  halt\n}",
        sensor_values={2: 1},
    )
    assert r["status"] == "success"
    assert r["simulation"]["relay_state"][200] == 1  # file0 激活


def test_pc_device_capability_gate():
    r = execute_lir("task t {\n  set file0 = 1\n  halt\n}")
    assert r["status"] == "compile_error"
    assert r["error"]["error_code"] == "INVALID_CAPABILITY"


# ---------------------------------------------------------------------------
# 三、行动平面：文件槽真实写入 + 安全边界
# ---------------------------------------------------------------------------

@pytest.fixture
def sandbox(tmp_path, monkeypatch):
    import pc_resources
    monkeypatch.setattr(pc_resources, "_FILE_ROOT", tmp_path.resolve())
    return tmp_path


FILE_LIR = "task log {\n  require cap(file0)\n  set file0 = 1\n  halt\n}"


def test_real_file_write(sandbox):
    r = execute_lir_with_side_effects(
        FILE_LIR,
        resource_bindings={"file0": {"path": "out.log", "content": "hello"}},
    )
    assert r["status"] == "success"
    assert r["materialized"]["ok"] is True
    assert (sandbox / "out.log").read_text(encoding="utf-8") == "hello"


def test_path_traversal_blocked(sandbox):
    r = execute_lir_with_side_effects(
        FILE_LIR,
        resource_bindings={"file0": {"path": "../../etc/passwd", "content": "x"}},
    )
    assert r["status"] == "side_effect_error"
    assert "escapes sandbox" in r["materialized"]["error"]
    assert not (sandbox.parent.parent / "etc" / "passwd").exists()


def test_file_slot_without_binding_refused(sandbox):
    r = execute_lir_with_side_effects(FILE_LIR, resource_bindings={})
    assert r["status"] == "side_effect_error"
    assert "no trusted binding" in r["materialized"]["error"]


def test_proc_slot_refused(sandbox):
    r = execute_lir_with_side_effects(
        "task p {\n  require cap(proc0)\n  set proc0 = 1\n  halt\n}",
        resource_bindings={},
    )
    assert r["status"] == "side_effect_error"
    assert "not supported" in r["materialized"]["error"]


def test_compile_failure_produces_no_side_effect(sandbox):
    # 编译失败时绝不物化
    r = execute_lir_with_side_effects(
        "task t {\n  set file0 = 1\n  halt\n}",  # 缺 require cap
        resource_bindings={"file0": {"path": "should_not_exist.log", "content": "x"}},
    )
    assert r["status"] == "compile_error"
    assert r["materialized"]["ok"] is False
    assert not (sandbox / "should_not_exist.log").exists()


# ---------------------------------------------------------------------------
# 四、动态内容：从传感器读数写入文件
# ---------------------------------------------------------------------------

def test_dynamic_content_from_sensor(sandbox):
    """读取温度传感器(device_id=2)的值，写入 file0(device_id=200)"""
    lir = (
        "task log_temp {\n"
        "  require cap(temperature_sensor)\n"
        "  require cap(file0)\n"
        "  read temperature_sensor\n"  # 读温度，值入栈
        "  set file0 = 1\n"            # 激活 file0
        "  halt\n}"
    )
    r = execute_lir_with_side_effects(
        lir,
        resource_bindings={"file0": {"path": "temp.log", "source_device": 2}},
        sensor_values={2: 25},  # 温度=25
    )
    assert r["status"] == "success"
    assert r["materialized"]["ok"] is True
    assert (sandbox / "temp.log").read_text(encoding="utf-8") == "25"


def test_dynamic_content_from_relay(sandbox):
    """读取继电器状态(device_id=5)，写入 file0"""
    lir = (
        "task log_relay {\n"
        "  require cap(relay1)\n"
        "  require cap(file0)\n"
        "  set relay1 = 1\n"   # 开继电器
        "  set file0 = 1\n"   # 激活 file0
        "  halt\n}"
    )
    r = execute_lir_with_side_effects(
        lir,
        resource_bindings={"file0": {"path": "relay.log", "source_device": 5}},
    )
    assert r["status"] == "success"
    assert r["materialized"]["ok"] is True
    assert (sandbox / "relay.log").read_text(encoding="utf-8") == "1"


def test_dynamic_content_source_device_missing(sandbox):
    """source_device 指向的设备不在 device_state 中"""
    lir = "task t {\n  require cap(file0)\n  set file0 = 1\n  halt\n}"
    r = execute_lir_with_side_effects(
        lir,
        resource_bindings={"file0": {"path": "out.log", "source_device": 999}},
    )
    assert r["status"] == "side_effect_error"
    assert "not found in device_state" in r["materialized"]["error"]


def test_static_content_backward_compatible(sandbox):
    """静态 content 仍然向后兼容"""
    lir = "task t {\n  require cap(file0)\n  set file0 = 1\n  halt\n}"
    r = execute_lir_with_side_effects(
        lir,
        resource_bindings={"file0": {"path": "out.log", "content": "static_value"}},
    )
    assert r["status"] == "success"
    assert (sandbox / "out.log").read_text(encoding="utf-8") == "static_value"
