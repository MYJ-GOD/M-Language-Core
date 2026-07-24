#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


M_LIT = 30
M_LT = 40
M_GT = 41
M_LE = 42
M_GE = 43
M_EQ = 44
M_ADD = 50
M_SUB = 51
M_IOW = 70
M_IOR = 71
M_GTWAY = 80
M_WAIT = 81
M_HALT = 82
M_JMP = 100
M_JZ = 101
M_DUP = 64
M_DRP = 65

DEVICE_IDS = {
    "water_sensor": 1,
    "temperature_sensor": 2,
    "humidity_sensor": 3,
    "relay1": 5,
    "relay2": 6,
}

DEFAULT_SENSOR_VALUES = {
    1: 42,
    2: 24,
    3: 55,
}

DEFAULT_RELAY_STATE = {
    5: 0,
    6: 0,
}


@dataclass
class SimResult:
    verify_pass: bool
    execution_pass: bool
    stage: str
    error_code: str
    message: str
    steps: int
    result_top: Optional[int]
    relay_state: Dict[int, int]

    def signature(self) -> Tuple[bool, Optional[int], Tuple[Tuple[int, int], ...]]:
        return (
            self.execution_pass,
            self.result_top,
            tuple(sorted(self.relay_state.items())),
        )


def decode_uvarint(buf: bytes, pos: int) -> Tuple[int, int]:
    res = 0
    shift = 0
    i = pos
    while i < len(buf):
        b = buf[i]
        i += 1
        res |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            return res, i
        shift += 7
        if shift >= 64:
            raise ValueError("varint overflow")
    raise ValueError("truncated varint")


def decode_zigzag64(n: int) -> int:
    return (n >> 1) ^ -(n & 1)


def decode_svarint(buf: bytes, pos: int) -> Tuple[int, int]:
    raw, pos = decode_uvarint(buf, pos)
    return int(decode_zigzag64(raw)), pos


def decode_program(payload: bytes) -> Tuple[bool, str, str, List[Tuple[int, Optional[int]]]]:
    program: List[Tuple[int, Optional[int]]] = []
    try:
        pos = 0
        while pos < len(payload):
            op, pos = decode_uvarint(payload, pos)
            if op == M_LIT:
                arg, pos = decode_uvarint(payload, pos)
                program.append((op, int(decode_zigzag64(arg))))
            elif op in (M_GTWAY, M_IOW, M_IOR, M_WAIT):
                arg, pos = decode_uvarint(payload, pos)
                if op != M_WAIT and arg > 255:
                    return False, "VERIFY_BAD_ENCODING", "device/capability id out of range", []
                program.append((op, int(arg)))
            elif op in (M_JMP, M_JZ):
                arg, pos = decode_svarint(payload, pos)
                program.append((op, int(arg)))
            elif op in (M_HALT, M_EQ, M_LT, M_GT, M_LE, M_GE, M_ADD, M_SUB, M_DUP, M_DRP):
                program.append((op, None))
            else:
                return False, "VERIFY_BAD_ENCODING", f"unsupported opcode {op}", []
        return True, "", "", program
    except ValueError as exc:
        return False, "VERIFY_BAD_ENCODING", str(exc), []


def verify_subset(payload: bytes) -> Tuple[bool, str, str]:
    ok, code, msg, program = decode_program(payload)
    if not ok:
        return False, code, msg
    for idx, (op, arg) in enumerate(program):
        if op in (M_JMP, M_JZ):
            assert arg is not None
            target = idx + 1 + arg
            if target < 0 or target >= len(program):
                return False, "VERIFY_BAD_ENCODING", f"jump target out of range at instruction {idx}"
    return True, "", ""


def simulate_subset(
    payload: bytes,
    sensor_values: Optional[Dict[int, int]] = None,
    relay_state: Optional[Dict[int, int]] = None,
    step_limit: Optional[int] = None,
    max_stack: Optional[int] = None,
) -> SimResult:
    ok, code, msg, program = decode_program(payload)
    if not ok:
        return SimResult(False, False, "verify", code, msg, 0, None, dict(DEFAULT_RELAY_STATE))

    sensors = dict(DEFAULT_SENSOR_VALUES)
    if sensor_values:
        sensors.update(sensor_values)
    relays = dict(DEFAULT_RELAY_STATE)
    if relay_state:
        relays.update(relay_state)

    stack: List[int] = []
    caps = set()
    ip = 0
    steps = 0

    try:
        while ip < len(program):
            # Step-limit guard (formal-model StepGuard): bounds total executed
            # instructions, preventing infinite loops from backward JMP/JZ.
            if step_limit is not None and steps >= step_limit:
                return SimResult(True, False, "execute", "EXEC_FAULT_STEP_LIMIT", "step limit exceeded", steps, stack[-1] if stack else None, relays)
            op, arg = program[ip]
            steps += 1

            # Stack-overflow guard (formal-model STK fault).
            if max_stack is not None and len(stack) > max_stack:
                return SimResult(True, False, "execute", "EXEC_FAULT_STACK_OVERFLOW", "stack overflow", steps, stack[-1] if stack else None, relays)

            if op == M_GTWAY:
                dev = int(arg)
                caps.add(dev)
                ip += 1
                continue

            if op == M_LIT:
                stack.append(int(arg))
                ip += 1
                continue

            if op == M_IOW:
                dev = int(arg)
                if dev not in caps:
                    return SimResult(True, False, "execute", "EXEC_FAULT_UNAUTHORIZED_IO", "unauthorized write", steps, stack[-1] if stack else None, relays)
                if not stack:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing write value", steps, None, relays)
                val = stack.pop()
                relays[dev] = val
                ip += 1
                continue

            if op == M_IOR:
                dev = int(arg)
                if dev not in caps:
                    return SimResult(True, False, "execute", "EXEC_FAULT_UNAUTHORIZED_IO", "unauthorized read", steps, stack[-1] if stack else None, relays)
                if dev in relays:
                    stack.append(relays[dev])
                else:
                    stack.append(sensors.get(dev, 0))
                ip += 1
                continue

            if op == M_WAIT:
                ip += 1
                continue

            if op == M_HALT:
                return SimResult(True, True, "success", "", "", steps, stack[-1] if stack else None, relays)

            if op == M_EQ:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing eq operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(1 if a == b else 0)
                ip += 1
                continue

            if op == M_LT:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing lt operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(1 if a < b else 0)
                ip += 1
                continue

            if op == M_GT:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing gt operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(1 if a > b else 0)
                ip += 1
                continue

            if op == M_LE:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing le operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(1 if a <= b else 0)
                ip += 1
                continue

            if op == M_GE:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing ge operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(1 if a >= b else 0)
                ip += 1
                continue

            if op == M_ADD:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing add operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(a + b)
                ip += 1
                continue

            if op == M_SUB:
                if len(stack) < 2:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing sub operands", steps, stack[-1] if stack else None, relays)
                b = stack.pop()
                a = stack.pop()
                stack.append(a - b)
                ip += 1
                continue

            if op == M_DUP:
                if not stack:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing dup operand", steps, None, relays)
                stack.append(stack[-1])
                ip += 1
                continue

            if op == M_DRP:
                if not stack:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing drop operand", steps, None, relays)
                stack.pop()
                ip += 1
                continue

            if op == M_JZ:
                if not stack:
                    return SimResult(True, False, "execute", "EXEC_FAULT_STACK_UNDERFLOW", "missing jz operand", steps, None, relays)
                cond = stack.pop()
                if cond == 0:
                    ip = ip + 1 + int(arg)
                else:
                    ip += 1
                continue

            if op == M_JMP:
                ip = ip + 1 + int(arg)
                continue

            return SimResult(True, False, "execute", "EXEC_FAULT_BAD_OPCODE", f"unknown opcode {op}", steps, stack[-1] if stack else None, relays)

        return SimResult(True, True, "success", "", "", steps, stack[-1] if stack else None, relays)
    except ValueError as exc:
        return SimResult(True, False, "execute", "EXEC_FAULT_BAD_ENCODING", str(exc), steps, stack[-1] if stack else None, relays)
