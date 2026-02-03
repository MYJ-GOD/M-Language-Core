# M-Language 使用示例

## 示例列表

| # | 名称 | 描述 |
|---|------|------|
| 01 | [Water Level Demo](./01_water_level_demo/) | 水位监测与继电器控制 |
| 02 | [LED Blink](./02_led_blink/) | LED 闪烁控制 |
| 03 | [Temperature Monitor](./03_temperature_monitor/) | 温度监控与报警 |

## 目录结构

```
Examples/
├── 01_water_level_demo/
│   ├── system_prompt.txt     # AI 提示词
│   ├── expected_tokens.json  # 期望的 M-Token 输出
│   └── README.md             # 示例说明
├── 02_led_blink/
├── 03_temperature_monitor/
└── README.md                 # 本文件
```

## 使用方法

1. 复制 `system_prompt.txt` 的内容作为 AI 的 system prompt
2. 用户提出需求，AI 根据提示词生成 M-Token
3. 比对 AI 生成的 Token 与 `expected_tokens.json`
4. 使用 MCP Server 的 `execute_m_logic` Skill 执行

## 验证工具

```bash
# 验证 Token 格式
python validate_tokens.py expected_tokens.json

# 生成测试报告
python test_example.py 01_water_level_demo
```

## 相关文档

- [MCP & Skills 规范](../Docs/mcp和skills.md)
- [M-Token 规范](../Docs/M-Token规范.md)

