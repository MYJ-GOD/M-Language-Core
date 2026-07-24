#!/usr/bin/env python3
"""
Minimal M-IR compiler for the current ccfc experiment subset.

Supported statements:
  - require cap(<device>)
  - set <device> = <0|1>
  - read <device>
  - wait <ms>ms
  - halt
  - readback <device> expect <value>
  - retry <n> times { ... }

Current lowering status:
  - readback lowers to executable bytecode via `IOR + LIT + EQ`
  - retry lowers to executable bytecode control flow via `LIT/DUP/JZ/JMP/SUB/DRP`

Additional host-side representation:
  - compile_to_plan(...) preserves structured execution-plan nodes for debugging
    and future board-side orchestration
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional


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

WRITABLE_DEVICES = {"relay1", "relay2"}


class CompilerError(Exception):
    def __init__(self, code: str, message: str, line_no: Optional[int] = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.line_no = line_no

    def to_dict(self) -> dict:
        return {
            "ok": False,
            "error_code": self.code,
            "message": self.message,
            "line": self.line_no,
        }


@dataclass
class Statement:
    kind: str
    line_no: int
    device: Optional[str] = None
    value: Optional[int] = None
    count: Optional[int] = None
    body: Optional[List["Statement"]] = None
    # For if/else
    condition_device: Optional[str] = None
    condition_op: Optional[str] = None
    condition_value: Optional[int] = None
    else_body: Optional[List["Statement"]] = None


@dataclass
class Program:
    task_name: str
    requirements: List[str]
    statements: List[Statement]


@dataclass
class IrInstr:
    kind: str
    opcode: Optional[int] = None
    arg: Optional[int] = None
    label: Optional[str] = None


def encode_uvarint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def encode_zigzag64(n: int) -> int:
    return ((n << 1) ^ (n >> 63)) & 0xFFFFFFFFFFFFFFFF


def op(code: int) -> bytes:
    return encode_uvarint(code)


def m_lit(value: int) -> bytes:
    return op(M_LIT) + encode_uvarint(encode_zigzag64(value))


def encode_svarint(n: int) -> bytes:
    return encode_uvarint(encode_zigzag64(n))


def normalize_source(source: str) -> List[tuple[int, str]]:
    lines: List[tuple[int, str]] = []
    for line_no, raw in enumerate(source.splitlines(), start=1):
        if line_no == 1:
            raw = raw.lstrip("\ufeff")
        stripped = raw.split("#", 1)[0].strip()
        if stripped:
            lines.append((line_no, stripped))
    return lines


def parse_program(source: str) -> Program:
    lines = normalize_source(source)
    if not lines:
        raise CompilerError("MIR_PARSE_ERROR", "empty input")

    first_line_no, first = lines[0]
    match = re.fullmatch(r"task\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", first)
    if not match:
        raise CompilerError(
            "MIR_PARSE_ERROR",
            "program must start with 'task <name> {'",
            first_line_no,
        )
    task_name = match.group(1)

    requirements: List[str] = []
    body_lines = lines[1:]
    if not body_lines:
        raise CompilerError("MIR_PARSE_ERROR", "missing task body", first_line_no)
    if body_lines[-1][1] != "}":
        raise CompilerError(
            "MIR_PARSE_ERROR",
            "task block must end with '}'",
            body_lines[-1][0],
        )

    statements, _ = parse_block(body_lines[:-1], 0, allow_require=True, requirements=requirements)
    return Program(task_name=task_name, requirements=requirements, statements=statements)


def parse_block(
    lines: List[tuple[int, str]],
    start_index: int,
    allow_require: bool,
    requirements: List[str],
    stop_on_else: bool = False,
) -> tuple[List[Statement], int]:
    statements: List[Statement] = []
    i = start_index
    while i < len(lines):
        line_no, line = lines[i]

        # Let caller handle "} else {" or "}"
        if stop_on_else and line in ("}", "} else {"):
            return statements, i
        if line == "}":
            return statements, i + 1

        requirement = re.fullmatch(r"require\s+cap\(([A-Za-z_][A-Za-z0-9_]*)\)", line)
        if requirement:
            if not allow_require:
                raise CompilerError(
                    "MIR_PARSE_ERROR",
                    "require cap(...) is only allowed at task scope",
                    line_no,
                )
            requirements.append(requirement.group(1))
            i += 1
            continue

        retry_match = re.fullmatch(r"retry\s+(\d+)\s+times\s*\{", line)
        if retry_match:
            count = int(retry_match.group(1))
            nested, next_index = parse_block(lines, i + 1, allow_require=False, requirements=requirements)
            statements.append(
                Statement(
                    kind="retry",
                    line_no=line_no,
                    count=count,
                    body=nested,
                )
            )
            i = next_index
            continue

        # if read(device) op value then { ... } else { ... }
        if_match = re.fullmatch(
            r"if\s+read\(([A-Za-z_][A-Za-z0-9_]*)\)\s*(>|<|>=|<=|==|!=)\s*(\d+)\s+then\s*\{",
            line,
        )
        if if_match:
            cond_device = if_match.group(1)
            cond_op = if_match.group(2)
            cond_value = int(if_match.group(3))
            then_body, next_index = parse_block(lines, i + 1, allow_require=False, requirements=requirements, stop_on_else=True)
            # Check for else block
            else_body = None
            if next_index < len(lines):
                _, next_line = lines[next_index]
                if next_line == "} else {":
                    else_body, next_index = parse_block(lines, next_index + 1, allow_require=False, requirements=requirements)
                elif next_line == "}":
                    next_index += 1  # consume closing brace
            statements.append(
                Statement(
                    kind="if",
                    line_no=line_no,
                    condition_device=cond_device,
                    condition_op=cond_op,
                    condition_value=cond_value,
                    body=then_body,
                    else_body=else_body,
                )
            )
            i = next_index
            continue

        # repeat N times { ... }
        repeat_match = re.fullmatch(r"repeat\s+(\d+)\s+times\s*\{", line)
        if repeat_match:
            count = int(repeat_match.group(1))
            nested, next_index = parse_block(lines, i + 1, allow_require=False, requirements=requirements)
            statements.append(
                Statement(
                    kind="repeat",
                    line_no=line_no,
                    count=count,
                    body=nested,
                )
            )
            i = next_index
            continue

        if re.fullmatch(r"set\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\d+)", line):
            set_match = re.fullmatch(r"set\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\d+)", line)
            assert set_match is not None
            statements.append(
                Statement(
                    kind="set",
                    line_no=line_no,
                    device=set_match.group(1),
                    value=int(set_match.group(2)),
                )
            )
            i += 1
            continue

        if re.fullmatch(r"read\s+([A-Za-z_][A-Za-z0-9_]*)", line):
            read_match = re.fullmatch(r"read\s+([A-Za-z_][A-Za-z0-9_]*)", line)
            assert read_match is not None
            statements.append(
                Statement(kind="read", line_no=line_no, device=read_match.group(1))
            )
            i += 1
            continue

        if re.fullmatch(r"wait\s+(\d+)ms", line):
            wait_match = re.fullmatch(r"wait\s+(\d+)ms", line)
            assert wait_match is not None
            statements.append(
                Statement(kind="wait", line_no=line_no, value=int(wait_match.group(1)))
            )
            i += 1
            continue

        if re.fullmatch(r"readback\s+([A-Za-z_][A-Za-z0-9_]*)\s+expect\s+(\d+)", line):
            rb_match = re.fullmatch(r"readback\s+([A-Za-z_][A-Za-z0-9_]*)\s+expect\s+(\d+)", line)
            assert rb_match is not None
            statements.append(
                Statement(
                    kind="readback",
                    line_no=line_no,
                    device=rb_match.group(1),
                    value=int(rb_match.group(2)),
                )
            )
            i += 1
            continue

        if line == "halt":
            statements.append(Statement(kind="halt", line_no=line_no))
            i += 1
            continue

        raise CompilerError("MIR_PARSE_ERROR", f"unrecognized statement: {line}", line_no)

    return statements, i


def validate_device(device: str, line_no: int) -> None:
    if device not in DEVICE_IDS:
        raise CompilerError("UNKNOWN_DEVICE", f"unknown device: {device}", line_no)


def compile_program(program: Program) -> bytes:
    required = set()
    ir: List[IrInstr] = []

    for device in program.requirements:
        validate_device(device, line_no=0)
        device_id = DEVICE_IDS[device]
        required.add(device)
        ir.append(IrInstr(kind="op", opcode=M_GTWAY, arg=device_id))

    state = {"label_counter": 0}
    for stmt in program.statements:
        ir.extend(lower_statement(stmt, required, state))

    return assemble_ir(ir)


def body_stack_delta(body: List[Statement]) -> int:
    """Estimate net stack push/pop for a list of statements."""
    delta = 0
    for stmt in body:
        if stmt.kind == "set":
            delta += 0   # LIT(+1) IOW(-1)
        elif stmt.kind == "read":
            delta += 1   # IOR(+1)
        elif stmt.kind == "wait":
            delta += 0
        elif stmt.kind == "halt":
            delta += 0
        elif stmt.kind == "readback":
            delta += 1   # IOR(+1) LIT(+1) EQ(-1)
        elif stmt.kind == "retry":
            delta += 1   # result boolean left on stack
        elif stmt.kind == "repeat":
            delta += body_stack_delta(stmt.body) if stmt.body else 0
        elif stmt.kind == "if":
            delta += body_stack_delta(stmt.body) if stmt.body else 0
    return delta


def next_label(state: dict, prefix: str) -> str:
    value = state["label_counter"]
    state["label_counter"] += 1
    return f"{prefix}_{value}"


def lower_statement(stmt: Statement, required: set[str], state: dict) -> List[IrInstr]:
    if stmt.kind == "set":
        assert stmt.device is not None and stmt.value is not None
        validate_device(stmt.device, stmt.line_no)
        if stmt.device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.device}' used without require cap(...)",
                stmt.line_no,
            )
        if stmt.device not in WRITABLE_DEVICES:
            raise CompilerError(
                "INVALID_SET_TARGET",
                f"device '{stmt.device}' is not writable",
                stmt.line_no,
            )
        if stmt.value not in (0, 1):
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"set value must be 0 or 1, got {stmt.value}",
                stmt.line_no,
            )
        return [
            IrInstr(kind="op", opcode=M_LIT, arg=stmt.value),
            IrInstr(kind="op", opcode=M_IOW, arg=DEVICE_IDS[stmt.device]),
        ]

    if stmt.kind == "read":
        assert stmt.device is not None
        validate_device(stmt.device, stmt.line_no)
        if stmt.device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.device}' used without require cap(...)",
                stmt.line_no,
            )
        return [IrInstr(kind="op", opcode=M_IOR, arg=DEVICE_IDS[stmt.device])]

    if stmt.kind == "wait":
        assert stmt.value is not None
        if stmt.value < 0:
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"wait value must be non-negative, got {stmt.value}",
                stmt.line_no,
            )
        return [IrInstr(kind="op", opcode=M_WAIT, arg=stmt.value)]

    if stmt.kind == "halt":
        return [IrInstr(kind="op", opcode=M_HALT)]

    if stmt.kind == "readback":
        assert stmt.device is not None and stmt.value is not None
        validate_device(stmt.device, stmt.line_no)
        if stmt.device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.device}' used without require cap(...)",
                stmt.line_no,
            )
        return [
            IrInstr(kind="op", opcode=M_IOR, arg=DEVICE_IDS[stmt.device]),
            IrInstr(kind="op", opcode=M_LIT, arg=stmt.value),
            IrInstr(kind="op", opcode=M_EQ),
        ]

    if stmt.kind == "retry":
        assert stmt.count is not None and stmt.body is not None
        if stmt.count < 1:
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"retry count must be positive, got {stmt.count}",
                stmt.line_no,
            )
        if not stmt.body:
            raise CompilerError(
                "INVALID_ARGUMENT",
                "retry body must not be empty",
                stmt.line_no,
            )
        loop_label = next_label(state, "retry_loop")
        success_label = next_label(state, "retry_success")
        fail_body_label = next_label(state, "retry_fail_body")
        exhausted_label = next_label(state, "retry_exhausted")
        done_label = next_label(state, "retry_done")

        body_ir: List[IrInstr] = []
        for child in stmt.body:
            if child.kind == "halt":
                raise CompilerError(
                    "UNSUPPORTED_CONSTRUCT",
                    "halt is not allowed inside retry body",
                    child.line_no,
                )
            body_ir.extend(lower_statement(child, required, state))
        if stmt.body[-1].kind not in {"readback", "retry"}:
            raise CompilerError(
                "UNSUPPORTED_CONSTRUCT",
                "retry body must end with readback or nested retry so it yields a boolean result",
                stmt.body[-1].line_no,
            )

        return [
            IrInstr(kind="op", opcode=M_LIT, arg=stmt.count),
            IrInstr(kind="label", label=loop_label),
            IrInstr(kind="op", opcode=M_DUP),
            IrInstr(kind="jump", opcode=M_JZ, label=exhausted_label),
            *body_ir,
            IrInstr(kind="jump", opcode=M_JZ, label=fail_body_label),  # body result == 0 => fail branch
            IrInstr(kind="jump", opcode=M_JMP, label=success_label),
            IrInstr(kind="label", label=fail_body_label),
            IrInstr(kind="op", opcode=M_LIT, arg=1),
            IrInstr(kind="op", opcode=M_SUB),
            IrInstr(kind="op", opcode=M_DUP),
            IrInstr(kind="jump", opcode=M_JZ, label=exhausted_label),
            IrInstr(kind="jump", opcode=M_JMP, label=loop_label),
            IrInstr(kind="label", label=success_label),
            IrInstr(kind="op", opcode=M_DRP),
            IrInstr(kind="op", opcode=M_LIT, arg=1),
            IrInstr(kind="jump", opcode=M_JMP, label=done_label),
            IrInstr(kind="label", label=exhausted_label),
            IrInstr(kind="op", opcode=M_DRP),
            IrInstr(kind="op", opcode=M_LIT, arg=0),
            IrInstr(kind="label", label=done_label),
        ]

    if stmt.kind == "if":
        assert stmt.condition_device is not None and stmt.condition_op is not None and stmt.condition_value is not None
        assert stmt.body is not None
        validate_device(stmt.condition_device, stmt.line_no)
        if stmt.condition_device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.condition_device}' used without require cap(...)",
                stmt.line_no,
            )
        cond_op_map = {">": M_GT, "<": M_LT, ">=": M_GE, "<=": M_LE, "==": M_EQ, "!=": None}
        if stmt.condition_op not in cond_op_map:
            raise CompilerError(
                "UNSUPPORTED_CONSTRUCT",
                f"unsupported comparison operator: {stmt.condition_op}",
                stmt.line_no,
            )
        else_label = next_label(state, "if_else")
        done_label = next_label(state, "if_done")

        # Condition: IOR device, LIT threshold, CMP
        cond_ir: List[IrInstr] = [
            IrInstr(kind="op", opcode=M_IOR, arg=DEVICE_IDS[stmt.condition_device]),
            IrInstr(kind="op", opcode=M_LIT, arg=stmt.condition_value),
        ]
        if stmt.condition_op == "!=":
            cond_ir.append(IrInstr(kind="op", opcode=M_EQ))
            cond_ir.append(IrInstr(kind="op", opcode=M_LIT, arg=0))
            cond_ir.append(IrInstr(kind="op", opcode=M_EQ))
        else:
            cond_ir.append(IrInstr(kind="op", opcode=cond_op_map[stmt.condition_op]))

        # Then body
        then_ir: List[IrInstr] = []
        for child in stmt.body:
            then_ir.extend(lower_statement(child, required, state))

        # Else body
        else_ir: List[IrInstr] = []
        if stmt.else_body:
            for child in stmt.else_body:
                else_ir.extend(lower_statement(child, required, state))

        result: List[IrInstr] = [*cond_ir]
        if else_ir:
            result.append(IrInstr(kind="jump", opcode=M_JZ, label=else_label))
            result.extend(then_ir)
            result.append(IrInstr(kind="jump", opcode=M_JMP, label=done_label))
            result.append(IrInstr(kind="label", label=else_label))
            result.extend(else_ir)
        else:
            result.append(IrInstr(kind="jump", opcode=M_JZ, label=done_label))
            result.extend(then_ir)
        result.append(IrInstr(kind="label", label=done_label))
        return result

    if stmt.kind == "repeat":
        assert stmt.count is not None and stmt.body is not None
        if stmt.count < 1:
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"repeat count must be positive, got {stmt.count}",
                stmt.line_no,
            )
        loop_label = next_label(state, "repeat_loop")
        done_label = next_label(state, "repeat_done")

        body_ir: List[IrInstr] = []
        for child in stmt.body:
            if child.kind == "halt":
                raise CompilerError(
                    "UNSUPPORTED_CONSTRUCT",
                    "halt is not allowed inside repeat body",
                    child.line_no,
                )
            body_ir.extend(lower_statement(child, required, state))

        # Drop any values left on stack by the body (e.g. readback result)
        delta = body_stack_delta(stmt.body)
        cleanup: List[IrInstr] = [IrInstr(kind="op", opcode=M_DRP) for _ in range(delta)]

        return [
            IrInstr(kind="op", opcode=M_LIT, arg=stmt.count),
            IrInstr(kind="label", label=loop_label),
            IrInstr(kind="op", opcode=M_DUP),
            IrInstr(kind="jump", opcode=M_JZ, label=done_label),
            *body_ir,
            *cleanup,
            IrInstr(kind="op", opcode=M_LIT, arg=1),
            IrInstr(kind="op", opcode=M_SUB),
            IrInstr(kind="jump", opcode=M_JMP, label=loop_label),
            IrInstr(kind="label", label=done_label),
            IrInstr(kind="op", opcode=M_DRP),
        ]

    raise CompilerError(
        "LOWERING_ERROR",
        f"unknown statement kind: {stmt.kind}",
        stmt.line_no,
    )


def instruction_size(instr: IrInstr) -> int:
    if instr.kind == "label":
        return 0
    if instr.kind == "op":
        size = len(op(instr.opcode))
        if instr.arg is not None:
            if instr.opcode == M_LIT:
                size += len(encode_uvarint(encode_zigzag64(instr.arg)))
            else:
                size += len(encode_uvarint(instr.arg))
        return size
    if instr.kind == "jump":
        size = len(op(instr.opcode))
        size += len(encode_svarint(0))
        return size
    raise ValueError(f"unknown ir kind: {instr.kind}")


def assemble_ir(ir: List[IrInstr]) -> bytes:
    instr_positions: List[int] = []
    label_to_index: dict[str, int] = {}
    executable: List[IrInstr] = []
    for item in ir:
        if item.kind == "label":
            if item.label is None:
                raise ValueError("label instruction missing label")
            label_to_index[item.label] = len(executable)
            continue
        executable.append(item)

    out = bytearray()
    for idx, item in enumerate(executable):
        instr_positions.append(len(out))
        if item.kind == "op":
            out += op(item.opcode)
            if item.arg is not None:
                if item.opcode == M_LIT:
                    out += encode_uvarint(encode_zigzag64(item.arg))
                else:
                    out += encode_uvarint(item.arg)
            continue
        if item.kind == "jump":
            if item.label is None:
                raise ValueError("jump instruction missing label")
            target_index = label_to_index[item.label]
            rel = target_index - (idx + 1)
            out += op(item.opcode) + encode_svarint(rel)
            continue
        raise ValueError(f"unknown ir kind: {item.kind}")
    return bytes(out)


def compile_source(source: str) -> tuple[Program, bytes]:
    program = parse_program(source)
    payload = compile_program(program)
    return program, payload


def compile_to_plan(source: str) -> dict:
    program = parse_program(source)
    for device in program.requirements:
        validate_device(device, line_no=0)
    required = set(program.requirements)
    return {
        "task_name": program.task_name,
        "requirements": [
            {"device": device, "device_id": DEVICE_IDS[device]}
            for device in program.requirements
        ],
        "steps": [statement_to_plan(stmt, required) for stmt in program.statements],
    }


def statement_to_plan(stmt: Statement, required: set[str]) -> dict:
    if stmt.kind == "set":
        assert stmt.device is not None and stmt.value is not None
        validate_device(stmt.device, stmt.line_no)
        if stmt.device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.device}' used without require cap(...)",
                stmt.line_no,
            )
        if stmt.device not in WRITABLE_DEVICES:
            raise CompilerError(
                "INVALID_SET_TARGET",
                f"device '{stmt.device}' is not writable",
                stmt.line_no,
            )
        if stmt.value not in (0, 1):
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"set value must be 0 or 1, got {stmt.value}",
                stmt.line_no,
            )
        return {"kind": "set", "device": stmt.device, "device_id": DEVICE_IDS[stmt.device], "value": stmt.value}

    if stmt.kind == "read":
        assert stmt.device is not None
        validate_device(stmt.device, stmt.line_no)
        if stmt.device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.device}' used without require cap(...)",
                stmt.line_no,
            )
        return {"kind": "read", "device": stmt.device, "device_id": DEVICE_IDS[stmt.device]}

    if stmt.kind == "wait":
        assert stmt.value is not None
        if stmt.value < 0:
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"wait value must be non-negative, got {stmt.value}",
                stmt.line_no,
            )
        return {"kind": "wait", "ms": stmt.value}

    if stmt.kind == "halt":
        return {"kind": "halt"}

    if stmt.kind == "readback":
        assert stmt.device is not None and stmt.value is not None
        validate_device(stmt.device, stmt.line_no)
        if stmt.device not in required:
            raise CompilerError(
                "INVALID_CAPABILITY",
                f"device '{stmt.device}' used without require cap(...)",
                stmt.line_no,
            )
        return {
            "kind": "readback",
            "device": stmt.device,
            "device_id": DEVICE_IDS[stmt.device],
            "expect": stmt.value,
        }

    if stmt.kind == "retry":
        assert stmt.count is not None and stmt.body is not None
        if stmt.count < 1:
            raise CompilerError(
                "INVALID_ARGUMENT",
                f"retry count must be positive, got {stmt.count}",
                stmt.line_no,
            )
        return {
            "kind": "retry",
            "times": stmt.count,
            "body": [statement_to_plan(child, required) for child in stmt.body],
        }

    if stmt.kind == "if":
        assert stmt.condition_device is not None and stmt.condition_op is not None and stmt.condition_value is not None
        assert stmt.body is not None
        result = {
            "kind": "if",
            "condition": {
                "device": stmt.condition_device,
                "op": stmt.condition_op,
                "value": stmt.condition_value,
            },
            "then": [statement_to_plan(child, required) for child in stmt.body],
        }
        if stmt.else_body:
            result["else"] = [statement_to_plan(child, required) for child in stmt.else_body]
        return result

    if stmt.kind == "repeat":
        assert stmt.count is not None and stmt.body is not None
        return {
            "kind": "repeat",
            "times": stmt.count,
            "body": [statement_to_plan(child, required) for child in stmt.body],
        }

    raise CompilerError(
        "LOWERING_ERROR",
        f"unknown statement kind: {stmt.kind}",
        stmt.line_no,
    )


def ast_to_dict(program: Program) -> dict:
    return {
        "task_name": program.task_name,
        "requirements": program.requirements,
        "statements": [statement_to_dict(stmt) for stmt in program.statements],
    }


def statement_to_dict(stmt: Statement) -> dict:
    data = {"kind": stmt.kind, "line_no": stmt.line_no}
    if stmt.device is not None:
        data["device"] = stmt.device
    if stmt.value is not None:
        data["value"] = stmt.value
    if stmt.count is not None:
        data["count"] = stmt.count
    if stmt.body is not None:
        data["body"] = [statement_to_dict(child) for child in stmt.body]
    if stmt.condition_device is not None:
        data["condition_device"] = stmt.condition_device
    if stmt.condition_op is not None:
        data["condition_op"] = stmt.condition_op
    if stmt.condition_value is not None:
        data["condition_value"] = stmt.condition_value
    if stmt.else_body is not None:
        data["else_body"] = [statement_to_dict(child) for child in stmt.else_body]
    return data


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Compile minimal M-IR to M-bytecode.")
    parser.add_argument("input", help="path to .mir file")
    parser.add_argument("--out-bin", help="write compiled bytecode to this file")
    parser.add_argument("--emit-hex", action="store_true", help="print hex payload to stdout")
    parser.add_argument("--dump-ast", action="store_true", help="print parsed AST as JSON")
    parser.add_argument("--dump-plan", action="store_true", help="print host-side execution plan as JSON")
    parser.add_argument("--json", action="store_true", help="print machine-readable result JSON")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    source = Path(args.input).read_text(encoding="utf-8")
    program = None
    payload = None
    plan = None
    try:
        if args.dump_plan and not args.emit_hex and not args.out_bin and not args.json:
            plan = compile_to_plan(source)
            program = parse_program(source)
        else:
            program, payload = compile_source(source)
            if args.dump_plan or args.json:
                plan = compile_to_plan(source)
    except CompilerError as exc:
        if args.json:
            print(json.dumps(exc.to_dict(), ensure_ascii=False, indent=2))
        else:
            location = f" line {exc.line_no}" if exc.line_no else ""
            print(f"{exc.code}:{location} {exc.message}", file=sys.stderr)
        return 1

    if args.out_bin and payload is not None:
        Path(args.out_bin).write_bytes(payload)

    if args.dump_ast:
        print(json.dumps(ast_to_dict(program), ensure_ascii=False, indent=2))

    if args.dump_plan and not args.json:
        print(json.dumps(plan, ensure_ascii=False, indent=2))

    if args.json:
        result = {
            "ok": True,
            "task_name": program.task_name,
            "requirements": program.requirements,
        }
        if payload is not None:
            result["payload_hex"] = payload.hex()
            result["payload_bytes"] = len(payload)
        if args.dump_ast:
            result["ast"] = ast_to_dict(program)
        if plan is not None:
            result["execution_plan"] = plan
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif payload is not None and (args.emit_hex or not args.dump_ast):
        print(payload.hex())

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
