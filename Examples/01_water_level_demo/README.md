# 水位监控与继电器控制示例

## 场景

监控水位计，当水位超过阈值时自动开启继电器控制水泵。

## 设备

| ID | 类型 | 名称 |
|----|------|------|
| 1 | SENSOR | Water_Level |
| 5 | ACTUATOR | Relay_1 |

## 测试用例

| 文件 | 描述 |
|------|------|
| `read_water.json` | 读取水位计值 |
| `conditional_relay.json` | 条件分支测试 |
| `water_relay_timer.json` | 完整场景：监控 + 延时 + 控制 |

## 使用方法

```bash
# 验证 Token 格式
python validate.py water_relay_timer.json

# 预期输出
Valid: True
Opcode count: 18
Bytecode length: 27 bytes
```

## AI 提示词

使用 `system_prompt.txt` 作为 AI 的 system prompt。

