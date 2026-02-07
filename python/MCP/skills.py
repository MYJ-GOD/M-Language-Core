from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Dict


ToolFn = Callable[..., Dict[str, Any]]


@dataclass
class SkillEngine:
    """Workflow-level skills that orchestrate MCP tools."""

    device_self_check: ToolFn
    health_report: ToolFn
    recent_audit: ToolFn
    snapshot: ToolFn
    threshold_eval: ToolFn
    safety_control: ToolFn
    relay_all_off: ToolFn

    def run_patrol_skill(
        self,
        include_relay_write_check: bool = False,
        recent_limit: int = 20,
    ) -> Dict[str, Any]:
        self_check = self.device_self_check(
            include_relay_write_check=include_relay_write_check,
            timeout_ms=4000,
        )
        health = self.health_report(recent_limit=recent_limit)
        recent = self.recent_audit(limit=min(max(recent_limit, 1), 100))

        health_level = str(health.get("overall_health", "degraded"))
        if health_level == "ok":
            summary = "巡检通过，系统状态正常。"
        elif health_level == "degraded":
            summary = "巡检通过但有退化项，建议尽快处理告警。"
        else:
            summary = "巡检失败，建议先执行安全回落并排查硬件链路。"

        return {
            "status": "success",
            "skill": "patrol",
            "summary": summary,
            "overall_health": health_level,
            "self_check": self_check,
            "health_report": health,
            "recent_audit": {
                "count": recent.get("count", 0),
                "events": recent.get("events", []),
            },
            "next_steps": [
                "若 overall_health=failed，先执行 relay_all_off_mcp。",
                "若 open_circuit_count>0，执行 reset_guard_mcp 后复测。",
                "若温湿度长期为 0，检查 DHT 供电与数据线。",
            ],
        }

    def run_environment_skill(
        self,
        samples: int = 3,
        timeout_ms: int = 5000,
    ) -> Dict[str, Any]:
        snap = self.snapshot(samples=samples, sample_interval_ms=200, timeout_ms=timeout_ms)
        eval_result = self.threshold_eval(samples=samples, timeout_ms=timeout_ms)

        level = str(eval_result.get("overall_level", "WARN")) if eval_result.get("status") == "success" else "WARN"
        if level == "CRITICAL":
            summary = "环境处于 CRITICAL，建议立即执行安全联动。"
        elif level == "WARN":
            summary = "环境处于 WARN，建议提高采样频率并关注趋势。"
        else:
            summary = "环境状态正常。"

        return {
            "status": "success",
            "skill": "environment_assessment",
            "summary": summary,
            "level": level,
            "snapshot": snap,
            "threshold_evaluation": eval_result,
            "recommended_action": eval_result.get("recommendation"),
        }

    def run_safe_control_skill(
        self,
        strategy: str = "auto",
        require_confirmation: bool = True,
        safety_confirm: bool = False,
        relay_channel: int = 1,
        relay_on_duration_sec: int = 8,
        timeout_ms: int = 5000,
    ) -> Dict[str, Any]:
        plan = (strategy or "auto").strip().lower()
        if plan not in {"auto", "emergency_stop", "pulse"}:
            return {
                "status": "error",
                "skill": "safe_control",
                "error": f"strategy 不支持: {strategy}",
                "allowed": ["auto", "emergency_stop", "pulse"],
                "code": "BAD_ARG",
            }

        if plan == "emergency_stop":
            action = self.relay_all_off(timeout_ms=timeout_ms)
            return {
                "status": "success" if action.get("status") == "success" else "failed",
                "skill": "safe_control",
                "strategy": plan,
                "action": action,
                "summary": "已执行紧急停机（全继电器关闭）。",
            }

        if plan == "pulse":
            action = self.safety_control(
                action_mode="pulse_relay",
                force_action=True,
                critical_only=False,
                require_confirmation=require_confirmation,
                safety_confirm=safety_confirm,
                relay_channel=relay_channel,
                relay_on_duration_sec=relay_on_duration_sec,
                timeout_ms=timeout_ms,
            )
            return {
                "status": action.get("status"),
                "skill": "safe_control",
                "strategy": plan,
                "action": action,
                "summary": "已按 pulse 策略执行受控联动。",
            }

        # auto
        action = self.safety_control(
            action_mode="all_off",
            force_action=False,
            critical_only=True,
            require_confirmation=require_confirmation,
            safety_confirm=safety_confirm,
            timeout_ms=timeout_ms,
        )
        return {
            "status": action.get("status"),
            "skill": "safe_control",
            "strategy": plan,
            "action": action,
            "summary": "已按 auto 策略评估并执行（仅 CRITICAL 触发）。",
        }
