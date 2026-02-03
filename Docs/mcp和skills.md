# M-Language MCP Server & Skills 规范

针对 ESP8266 IoT 套装，结合 M 语言特性设计的「原子化能力 (Atomic Skills)」方案。

## 核心原则

**Skill 不负责业务逻辑，只暴露硬件原语** —— AI 像写脚本一样组合这些原语。

---

## 1. 架构与角色分工

| 组件 | 职责 |
|------|------|
| **Skills (定义侧)** | 告诉 AI 硬件有什么（设备清单、指令集） |
| **MCP Server (执行侧)** | 1. Validator 静态检查字节码<br>2. Varint 编码<br>3. GATEWAY 授权<br>4. 串口发送 & 接收结果 |

---

## 2. Core vs Extension 指令

### 重要规则

**AI 生成的 M-Token 必须只使用 Core 指令 (0-99)**

| 区间 | 用途 | AI 可用？ |
|------|------|----------|
| 0–99 | Core（冻结，ABI） | ✅ 必须使用 |
| 100–199 | Extension/IR | ❌ 禁止 |
| 200–239 | 平台/硬件扩展 | ⚠️ 仅限 ALLOC/FREE |

### Core 指令速查

```
控制流:  B(10), E(11), IF(12), WH(13), FR(14), FN(15), RT(16), CL(17)
数据:    LIT(30), V(31), LET(32), SET(33)
比较:    LT(40), GT(41), LE(42), GE(43), EQ(44)
算术:    ADD(50), SUB(51), MUL(52), DIV(53), AND(54), OR(55), XOR(56), SHL(57), SHR(58)
数组:    LEN(60), GET(61), PUT(62), SWP(63)
栈:      DUP(64), DRP(65), ROT(66)
IO:      IOW(70), IOR(71)
系统:    GTWAY(80), WAIT(81), HALT(82), TRACE(83)
```

---

## 3. 三个核心 Skill

### Skill A: get_hardware_topology

**作用**: 获取设备清单

**返回示例**:
```json
{
  "devices": [
    {"id": 1, "type": "SENSOR", "name": "Water_Level", "unit": "raw_0_1024"},
    {"id": 2, "type": "SENSOR", "name": "DHT11_Temp", "unit": "Celsius"},
    {"id": 5, "type": "ACTUATOR", "name": "Relay_1", "desc": "Controls water pump"},
    {"id": 6, "type": "ACTUATOR", "name": "Onboard_LED", "desc": "Indicator"}
  ],
  "auth_required": [70, 71]  // 需要 GATEWAY 授权的指令
}
```

---

### Skill B: execute_m_logic

**作用**: 下发执行指令（最关键环节）

**参数**:
| 字段 | 类型 | 说明 |
|------|------|------|
| `m_tokens` | int[] | M-Token 整数数组 |
| `description` | string | AI 对这段逻辑的自然语言解释（可选） |
| `timeout_ms` | int | 超时时间（默认 5000） |

**内部流程**:
```
Python 收到数组
    ↓
1. GATEWAY 授权 (如果用到 IO)
    ↓
2. Static Validator 检查 (opcode 合法性, 栈平衡, 跳转范围)
    ↓
3. Varint 编码
    ↓
4. 串口发送 (长度前缀 + 字节流)
    ↓
5. 等待 ESP8266 返回 HALT / FAULT / 超时
    ↓
返回: { status, fault_code, steps, result }
```

**参数压栈规则** (栈机模型):
- **IOR**: `<device_id>` 在栈顶 → `[LIT, device_id, IOR]` → 值压栈
- **IOW**: `<value>, <device_id>` 在栈顶 → `[LIT, value, LIT, device_id, IOW]`
- **WAIT**: `<milliseconds>` 在栈顶 → `[LIT, ms, WAIT]`

---

### Skill C: read_vm_state

**作用**: 读取 VM 执行状态

**参数**: `stack_depth` (可选，要读取的栈深度)

**返回示例**:
```json
{
  "completed": true,
  "fault": "NONE",
  "steps": 15,
  "sp": 0,
  "stack": [42],           // 栈顶值
  "fault_code": 0,
  "fault_string": "Success"
}
```

---

## 4. 执行全链路示例

### 用户意图

> "如果水位超过 500，开启继电器 10 秒后关闭"

### Step 1: AI 查询设备清单 (Skill A)

```json
{
  "devices": [
    {"id": 1, "type": "SENSOR", "name": "Water_Level", "unit": "raw_0_1024"},
    {"id": 5, "type": "ACTUATOR", "name": "Relay_1", "desc": "Controls water pump"}
  ]
}
```

### Step 2: AI 生成 M-Token (Core 指令)

```
[30, 1, 71,      // LIT 1, IOR  → 读取水位计 (device_id=1)
 30, 500, 41, 12, 10, 0, 11, 11,   // LIT 500, GT, IF  → 水位 > 500?
    [30, 1, 30, 5, 70,            //  真: LIT 1, LIT 5, IOW  → 开启继电器 (value=1, dev=5)
     30, 10000, 81,                //       LIT 10000, WAIT  → 等待 10 秒
     30, 0, 30, 5, 70],           //       LIT 0, LIT 5, IOW  → 关闭继电器
 11, 82]                          //  假分支结束, HALT
```

### Step 3: MCP Server 执行

```python
# 1. GATEWAY 授权
vm.caps[70] = 1  # 授权 IOW
vm.caps[71] = 1  # 授权 IOR

# 2. 静态验证
result = m_validate(bytecode)
if not result.valid:
    return {"error": result.msg}

# 3. Varint 编码后发送
serial.write(len(code))
serial.write(code)

# 4. 等待结果
response = serial.read_until(b'\x52')  # HALT opcode
```

### Step 4: 字节码详解

```
30 01       LIT 1           → 压入 device_id=1
71         IOR             → 从设备1读，压入水位值

30 F4 03    LIT 500         → 压入 500
41         GT              → 水位 > 500? (结果 0 或 1)
12         IF              → 条件分支
 0A        B               → 真分支开始
   30 01   LIT 1           → 压入 value=1
   30 05   LIT 5           → 压入 device_id=5
   70      IOW             → 开启继电器
   30 10 27 LIT 10000      → 压入 10000ms
   81      WAIT            → 等待 10 秒
   30 00   LIT 0           → 压入 value=0
   30 05   LIT 5           → 压入 device_id=5
   70      IOW             → 关闭继电器
0B        E               → 真分支结束
11        E               → IF 整体结束
52        HALT            → 停机
```

### Step 5: 执行结果

```json
{
  "completed": true,
  "fault": "NONE",
  "steps": 23,
  "sp": 0,
  "stack": [],
  "trace": [
    {"step": 5, "op": "IOR", "value": 520},
    {"step": 8, "op": "IOW", "dev": 5, "value": 1}
  ]
}
```

---

## 5. 故障码速查

| 码 | 名称 | 原因 |
|----|------|------|
| 0 | NONE | 正常完成 |
| 1 | STACK_OVERFLOW | 栈溢出 |
| 2 | STACK_UNDERFLOW | 栈下溢 |
| 9 | DIV_BY_ZERO | 除零 |
| 10 | INDEX_OOB | 数组越界 |
| 11 | BAD_ARG | 非法参数 |
| 15 | CALL_DEPTH_LIMIT | 调用深度超限 |
| 17 | UNAUTHORIZED | 未授权的 IO 操作 |

---

## 6. 技术限制

| 项目 | 值 |
|------|-----|
| 最大栈深度 | 256 |
| 最大调用深度 | 32 |
| 最大执行步数 | 1,000,000 |
| 整数类型 | i64 (ZigZag 编码) |
| 跳转偏移 | svarint (相对偏移) |

