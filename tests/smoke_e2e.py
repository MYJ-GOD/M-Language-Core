# 端到端冒烟：自然语言 -> LLM(Ollama) -> M-IR -> M-Token 字节码 -> 执行
#
# 验证核心命题：LLM 能否从自然语言生成"可编译、可执行、门控通过"的 M-IR。
# 这是产品优先路线的第一个真实里程碑（不再是构造好的 M-IR，而是 LLM 自己写的）。
#
# 用法：cd tests && python smoke_e2e.py [model]

import re
import sys
import json
import urllib.request
from pathlib import Path

# 添加 python/MCP 到路径
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python" / "MCP"))

from lir_backend import execute_lir

OLLAMA = "http://localhost:11434/api/chat"
MODEL = sys.argv[1] if len(sys.argv) > 1 else "qwen3:8b"

SYSTEM = """你是一个硬件控制程序生成器。用户用自然语言描述需求，你只输出一段 LIR 程序，用 ```lir 代码块包裹，不要任何解释。

LIR 语法：
task <名字> {
  require cap(<设备>)      # 用到的每个设备都必须先声明
  set <设备> = <0或1>       # 写设备（仅 relay1/relay2 可写）
  read <设备>              # 读设备
  wait <毫秒>ms            # 延时
  readback <设备> expect <值>  # 读回并期望某值
  retry <次数> times { ... }   # 重试循环
  halt                    # 结束
}

可用设备：water_sensor(水位), temperature_sensor(温度), humidity_sensor(湿度), relay1(继电器1/风扇), relay2(继电器2)。

规则：
1. set 的值只能是 0 或 1；只有 relay1/relay2 能被 set；用到的设备必须 require cap。
2. task 名字只能用英文字母/数字/下划线。
3. retry 循环体必须以 readback 结尾（readback 是循环的判定条件）；wait 要放在 readback 之前。

正确的 retry 示例（反复等待并检查温度）：
task watch_temp {
  require cap(temperature_sensor)
  retry 10 times {
    wait 1000ms
    readback temperature_sensor expect 25
  }
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
    with urllib.request.urlopen(req, timeout=180) as resp:
        data = json.loads(resp.read())
    return data["message"]["content"]


def extract_lir(text):
    # 优先取 ```lir 代码块，退化到任意代码块，再退化到 task {...}
    m = re.search(r"```(?:lir)?\s*(task\s+.*?\})\s*```", text, re.DOTALL)
    if m:
        return m.group(1).strip()
    m = re.search(r"(task\s+\w+\s*\{.*?\n\})", text, re.DOTALL)
    return m.group(1).strip() if m else None


CASES = [
    ("打开风扇", None),
    ("打开风扇，等两秒后关掉", None),
    ("盯着温度，反复检查直到到达 25 度，最多试 10 次", {2: 25}),
]


def main():
    print("模型:", MODEL)
    passed = 0
    for i, (prompt, sensors) in enumerate(CASES, 1):
        print("\n" + "=" * 60)
        print("[%d] 用户: %s" % (i, prompt))
        try:
            raw = ask_ollama(prompt)
        except Exception as e:
            print("  ollama 调用失败:", e)
            continue
        lir = extract_lir(raw)
        if not lir:
            print("  未能从回复中提取 LIR。原始回复前 200 字:\n", raw[:200])
            continue
        print("  LLM 生成的 LIR:")
        print("    " + lir.replace("\n", "\n    "))
        r = execute_lir(lir, sensor_values=sensors)
        print("  执行结果: status=%s" % r["status"])

        # 自修闭环：编译错误时把结构化错误回传给 LLM，让它改一次
        if r["status"] == "compile_error":
            print("    首轮编译错误: %s" % r["error"])
            fix_prompt = (
                "你上次生成的 LIR 编译失败了。\n原 LIR:\n%s\n\n"
                "编译器错误(JSON): %s\n\n"
                "请修正后重新只输出 ```lir 代码块。注意：task 名字只能用英文字母/数字/下划线。"
                % (lir, json.dumps(r["error"], ensure_ascii=False))
            )
            try:
                raw2 = ask_ollama(fix_prompt)
                lir2 = extract_lir(raw2)
                if lir2:
                    print("  自修后 LIR:")
                    print("    " + lir2.replace("\n", "\n    "))
                    r = execute_lir(lir2, sensor_values=sensors)
                    print("  自修后结果: status=%s" % r["status"])
            except Exception as e:
                print("    自修调用失败:", e)

        if r["status"] == "success":
            print("    字节码: %s" % r["bytecode_hex"])
            print("    设备终态: %s  步数: %s" % (r["simulation"]["relay_state"], r["simulation"]["steps"]))
            passed += 1
        elif r["status"] == "compile_error":
            print("    仍编译错误: %s" % r["error"])
        else:
            print("    执行故障: %s" % r["simulation"]["error_code"])
    print("\n" + "=" * 60)
    print("端到端通过: %d/%d" % (passed, len(CASES)))


if __name__ == "__main__":
    main()
