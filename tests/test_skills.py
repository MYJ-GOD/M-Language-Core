# Skills 层测试
#
# 使用 mock 测试巡检、环境评估、安全控制、继电器闭环四个工作流。
# 运行：cd tests && python -m pytest test_skills.py -v

import sys
from pathlib import Path
from unittest.mock import MagicMock

# 添加 python/MCP 到路径
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python" / "MCP"))

import pytest
from skills import SkillEngine


# ---------------------------------------------------------------------------
# Mock 工具函数
# ---------------------------------------------------------------------------

def make_mock_tools():
    """创建 mock 的 MCP 工具函数"""
    return {
        "device_self_check": MagicMock(return_value={
            "status": "success",
            "overall_health": "ok",
            "devices": {"water_sensor": "ok", "temperature_sensor": "ok"},
        }),
        "health_report": MagicMock(return_value={
            "status": "success",
            "overall_health": "ok",
            "checks": [],
        }),
        "recent_audit": MagicMock(return_value={
            "status": "success",
            "count": 5,
            "events": [{"tool": "read_water_level", "result": "success"}],
        }),
        "snapshot": MagicMock(return_value={
            "status": "success",
            "water_level": 42,
            "temperature": 25,
            "humidity": 55,
        }),
        "threshold_eval": MagicMock(return_value={
            "status": "success",
            "overall_level": "NORMAL",
            "recommendation": "No action needed.",
        }),
        "safety_control": MagicMock(return_value={
            "status": "success",
            "action": {"result": {"action_applied": True}},
        }),
        "relay_all_off": MagicMock(return_value={
            "status": "success",
            "channels": [0, 0],
        }),
        "relay_set_with_verify": MagicMock(return_value={
            "status": "success",
            "action_applied": True,
            "matched": True,
        }),
        "read_relay_state": MagicMock(return_value={
            "status": "success",
            "relay_state": {5: 0, 6: 0},
        }),
    }


@pytest.fixture
def engine():
    """创建 SkillEngine 实例，使用 mock 工具"""
    tools = make_mock_tools()
    return SkillEngine(**tools), tools


# ---------------------------------------------------------------------------
# 巡检技能
# ---------------------------------------------------------------------------

def test_patrol_skill_ok(engine):
    """巡检成功：系统健康"""
    eng, tools = engine
    r = eng.run_patrol_skill()
    assert r["status"] == "success"
    assert r["skill"] == "patrol"
    assert r["overall_health"] == "ok"
    assert "healthy" in r["summary"].lower()
    assert tools["device_self_check"].called
    assert tools["health_report"].called
    assert tools["recent_audit"].called


def test_patrol_skill_degraded(engine):
    """巡检降级：系统有警告"""
    eng, tools = engine
    tools["health_report"].return_value = {
        "status": "success",
        "overall_health": "degraded",
        "checks": [{"name": "serial", "status": "warn"}],
    }
    r = eng.run_patrol_skill()
    assert r["overall_health"] == "degraded"
    assert "warning" in r["summary"].lower() or "maintenance" in r["summary"].lower()


def test_patrol_skill_failed(engine):
    """巡检失败：系统不可用"""
    eng, tools = engine
    tools["health_report"].return_value = {
        "status": "success",
        "overall_health": "failed",
        "checks": [],
    }
    r = eng.run_patrol_skill()
    assert r["overall_health"] == "failed"
    assert "failed" in r["summary"].lower()


def test_patrol_skill_includes_audit(engine):
    """巡检结果包含审计摘要"""
    eng, tools = engine
    r = eng.run_patrol_skill(recent_limit=10)
    assert r["recent_audit"]["count"] == 5
    assert len(r["recent_audit"]["events"]) == 1


# ---------------------------------------------------------------------------
# 环境评估技能
# ---------------------------------------------------------------------------

def test_environment_skill_normal(engine):
    """环境正常"""
    eng, tools = engine
    r = eng.run_environment_skill()
    assert r["status"] == "success"
    assert r["skill"] == "environment_assessment"
    assert r["level"] == "NORMAL"
    assert "normal" in r["summary"].lower()
    assert tools["snapshot"].called
    assert tools["threshold_eval"].called


def test_environment_skill_warn(engine):
    """环境警告"""
    eng, tools = engine
    tools["threshold_eval"].return_value = {
        "status": "success",
        "overall_level": "WARN",
        "recommendation": "Increase sampling.",
    }
    r = eng.run_environment_skill()
    assert r["level"] == "WARN"
    assert "warn" in r["summary"].lower()


def test_environment_skill_critical(engine):
    """环境危险"""
    eng, tools = engine
    tools["threshold_eval"].return_value = {
        "status": "success",
        "overall_level": "CRITICAL",
        "recommendation": "Execute safety action immediately.",
    }
    r = eng.run_environment_skill()
    assert r["level"] == "CRITICAL"
    assert "critical" in r["summary"].lower()


def test_environment_skill_with_samples(engine):
    """环境评估支持多样本"""
    eng, tools = engine
    eng.run_environment_skill(samples=5, timeout_ms=10000)
    tools["snapshot"].assert_called_once_with(samples=5, sample_interval_ms=200, timeout_ms=10000)
    tools["threshold_eval"].assert_called_once_with(samples=5, timeout_ms=10000)


# ---------------------------------------------------------------------------
# 安全控制技能
# ---------------------------------------------------------------------------

def test_safe_control_auto(engine):
    """自动策略"""
    eng, tools = engine
    tools["safety_control"].return_value = {
        "status": "success",
        "action": {"result": {"status": "success"}},
    }
    r = eng.run_safe_control_skill(strategy="auto")
    assert r["status"] == "success"
    assert r["skill"] == "safe_control"
    assert r["strategy"] == "auto"
    assert r["action_applied"] is True


def test_safe_control_emergency_stop(engine):
    """紧急停止"""
    eng, tools = engine
    r = eng.run_safe_control_skill(strategy="emergency_stop")
    assert r["status"] == "success"
    assert r["strategy"] == "emergency_stop"
    assert tools["relay_all_off"].called


def test_safe_control_pulse(engine):
    """脉冲策略"""
    eng, tools = engine
    r = eng.run_safe_control_skill(strategy="pulse")
    assert r["status"] == "success"
    assert r["strategy"] == "pulse"
    assert tools["safety_control"].called


def test_safe_control_invalid_strategy(engine):
    """无效策略"""
    eng, tools = engine
    r = eng.run_safe_control_skill(strategy="invalid")
    assert r["status"] == "error"
    assert r["code"] == "BAD_ARG"
    assert "unsupported" in r["error"].lower()


# ---------------------------------------------------------------------------
# 继电器闭环技能
# ---------------------------------------------------------------------------

def test_relay_closed_loop_success(engine):
    """继电器闭环成功"""
    eng, tools = engine
    r = eng.run_relay_closed_loop_skill(channel=1, state=1)
    assert r["status"] == "success"
    assert r["skill"] == "relay_closed_loop"
    assert r["action_applied"] is True
    assert r["requested"] == {"channel": 1, "state": 1}
    assert tools["relay_set_with_verify"].called
    assert tools["read_relay_state"].called


def test_relay_closed_loop_failure_with_fallback(engine):
    """继电器闭环失败，触发安全回退"""
    eng, tools = engine
    tools["relay_set_with_verify"].return_value = {
        "status": "failed",
        "action_applied": False,
        "matched": False,
    }
    r = eng.run_relay_closed_loop_skill(channel=1, state=1, fail_safe_all_off=True)
    assert r["status"] == "failed"
    assert r["action_applied"] is False
    assert r["fallback_action"] is not None
    assert tools["relay_all_off"].called


def test_relay_closed_loop_failure_no_fallback(engine):
    """继电器闭环失败，不触发安全回退"""
    eng, tools = engine
    tools["relay_set_with_verify"].return_value = {
        "status": "failed",
        "action_applied": False,
        "matched": False,
    }
    r = eng.run_relay_closed_loop_skill(channel=1, state=1, fail_safe_all_off=False)
    assert r["status"] == "failed"
    assert r["fallback_action"] is None
    assert not tools["relay_all_off"].called
