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
    relay_set_with_verify: ToolFn
    read_relay_state: ToolFn

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
            summary = "Patrol passed and system is healthy."
        elif health_level == "degraded":
            summary = "Patrol passed with warnings; maintenance is recommended."
        else:
            summary = "Patrol failed; run safe fallback and inspect hardware chain."

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
                "If overall_health=failed, run relay_all_off_mcp first.",
                "If open circuits exist, run reset_guard_mcp and retest.",
                "If temperature/humidity readings stay invalid, inspect DHT wiring and power.",
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
            summary = "Environment is CRITICAL; execute safety action immediately."
        elif level == "WARN":
            summary = "Environment is WARN; increase sampling and observe trend."
        else:
            summary = "Environment is normal."

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
                "error": f"unsupported strategy: {strategy}",
                "allowed": ["auto", "emergency_stop", "pulse"],
                "code": "BAD_ARG",
            }

        if plan == "emergency_stop":
            action = self.relay_all_off(timeout_ms=timeout_ms)
            action_applied = bool(action.get("status") == "success")
            return {
                "status": "success" if action_applied else "failed",
                "skill": "safe_control",
                "strategy": plan,
                "action": action,
                "action_applied": action_applied,
                "summary": "Emergency stop executed.",
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
            action_result = action.get("action", {}).get("result", {}) if isinstance(action.get("action"), dict) else {}
            action_applied = bool(action_result.get("action_applied", action_result.get("matched", False)))
            return {
                "status": action.get("status"),
                "skill": "safe_control",
                "strategy": plan,
                "action": action,
                "action_applied": action_applied,
                "summary": "Pulse safety strategy executed.",
            }

        action = self.safety_control(
            action_mode="all_off",
            force_action=False,
            critical_only=True,
            require_confirmation=require_confirmation,
            safety_confirm=safety_confirm,
            timeout_ms=timeout_ms,
        )
        action_result = action.get("action", {}).get("result", {}) if isinstance(action.get("action"), dict) else {}
        action_applied = bool(action_result.get("status") == "success")
        return {
            "status": action.get("status"),
            "skill": "safe_control",
            "strategy": plan,
            "action": action,
            "action_applied": action_applied,
            "summary": "Auto safety strategy evaluated and executed.",
        }

    def run_relay_closed_loop_skill(
        self,
        channel: int,
        state: int,
        timeout_ms: int = 5000,
        retries: int = 2,
        safety_confirm: bool = False,
        fail_safe_all_off: bool = True,
    ) -> Dict[str, Any]:
        action = self.relay_set_with_verify(
            channel=channel,
            state=state,
            timeout_ms=timeout_ms,
            retries=retries,
            safety_confirm=safety_confirm,
        )

        applied = bool(action.get("action_applied", action.get("matched", False)))
        fallback = None
        if not applied and fail_safe_all_off:
            fallback = self.relay_all_off(timeout_ms=timeout_ms)

        state_view = self.read_relay_state(channel=0, timeout_ms=timeout_ms)

        return {
            "status": "success" if applied else "failed",
            "skill": "relay_closed_loop",
            "requested": {"channel": channel, "state": state},
            "action_result": action,
            "action_applied": applied,
            "fallback_action": fallback,
            "relay_state": state_view,
        }
