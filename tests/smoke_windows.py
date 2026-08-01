# Windows 控制端到端冒烟测试
#
# 验证：LLM 生成 M-IR → 编译 → 执行 → 真实写文件/起进程
#
# 用法：cd tests && python smoke_windows.py [model]

import os
import re
import sys
import json
import urllib.request
from pathlib import Path

# 沙箱根目录（绝对路径），必须在 import lir_backend 之前设好——
# pc_resources 在 import 时读取 MCP_LIR_FILE_ROOT 固定 _FILE_ROOT。
SANDBOX = Path(__file__).resolve().parent.parent / "data" / "lir_sandbox"
os.environ["MCP_LIR_FILE_ROOT"] = str(SANDBOX)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python" / "MCP"))

from lir_backend import execute_lir_with_side_effects

OLLAMA = "http://localhost:11434/api/chat"
MODEL = sys.argv[1] if len(sys.argv) > 1 else "qwen3:8b"

SYSTEM = """你是一个系统控制程序生成器。用户用自然语言描述需求，你只输出一段 M-IR 程序，用 ```lir 代码块包裹，不要任何解释。

M-IR 语法：
task <名字> {
  require cap(<槽位>)      # 用到的每个槽位都必须先声明
  set <槽位> = <0或1>       # 激活槽位（1=执行，0=跳过）
  halt                    # 结束
}

可用槽位：
- file0：写文件（服务端配置了路径和内容，LLM 只管激活）
- proc0：执行命令（服务端配置了命令，LLM 只管激活）

规则：
1. set 的值只能是 0 或 1
2. task 名字只能用英文字母/数字/下划线
3. 用到的槽位必须先 require cap

正确示例：
task write_log {
  require cap(file0)
  set file0 = 1
  halt
}"""


def ask_ollama(prompt):
    body = json.dumps({
        "model": MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": prompt},
        ],
        "stream": False,
        "think": False,
    }).encode("utf-8")
    req = urllib.request.Request(OLLAMA, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.loads(resp.read())
    return data["message"]["content"]


def extract_lir(text):
    m = re.search(r"```(?:lir)?\s*(task\s+.*?\})\s*```", text, re.DOTALL)
    if m:
        return m.group(1).strip()
    m = re.search(r"(task\s+\w+\s*\{.*?\n\})", text, re.DOTALL)
    return m.group(1).strip() if m else None


CASES = [
    {
        "prompt": "写一条日志到文件",
        "bindings": {
            "file0": {"path": "test.log", "content": "hello from M-IR on Windows"}
        },
        "verify": lambda r: (SANDBOX / "test.log").exists() and
                            "hello from M-IR" in (SANDBOX / "test.log").read_text(encoding="utf-8"),
        "verify_desc": "test.log 存在且内容正确",
    },
    {
        "prompt": "执行一个命令",
        "bindings": {
            "proc0": {"command": "python", "args": ["-c", "print('M-IR controlled Windows')"]}
        },
        "verify": lambda r: (r.get("materialized") or {}).get("processes", {}).get("results", [{}])[0].get("returncode", -1) == 0,
        "verify_desc": "进程退出码为 0",
    },
]


def main():
    SANDBOX.mkdir(parents=True, exist_ok=True)
    print("模型:", MODEL)
    print("沙箱:", SANDBOX)
    passed = 0

    for i, case in enumerate(CASES, 1):
        print("\n" + "=" * 60)
        print("[%d] 用户: %s" % (i, case["prompt"]))

        try:
            raw = ask_ollama(case["prompt"])
        except Exception as e:
            print("  Ollama 调用失败:", e)
            continue

        lir = extract_lir(raw)
        if not lir:
            print("  未能提取 M-IR，原始回复前 200 字:\n", raw[:200])
            continue

        print("  LLM 生成的 M-IR:")
        print("    " + lir.replace("\n", "\n    "))

        r = execute_lir_with_side_effects(lir, resource_bindings=case["bindings"])
        print("  执行结果: status=%s" % r["status"])

        if r["status"] == "compile_error":
            print("  编译错误:", r["error"])
            continue

        if r["status"] != "success":
            print("  执行失败:", r.get("materialized"))
            continue

        # 验证真实副作用
        try:
            ok = case["verify"](r)
        except Exception as e:
            ok = False
            print("  验证异常:", e)

        if ok:
            print("  [PASS] %s" % case["verify_desc"])
            passed += 1
        else:
            print("  [FAIL] %s" % case["verify_desc"])

    print("\n" + "=" * 60)
    print("Windows 控制通过: %d/%d" % (passed, len(CASES)))


if __name__ == "__main__":
    main()
