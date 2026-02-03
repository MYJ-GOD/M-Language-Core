# M-Language MCP Skills 实现

## 三个核心 Skill

### Skill A: get_hardware_topology

```python
def get_hardware_topology() -> dict:
    """获取设备清单"""
    return {
        "devices": [
            {"id": 1, "type": "SENSOR", "name": "Water_Level", "unit": "raw_0_1024"},
            {"id": 2, "type": "SENSOR", "name": "DHT11_Temp", "unit": "Celsius"},
            {"id": 5, "type": "ACTUATOR", "name": "Relay_1", "desc": "Controls water pump"},
            {"id": 6, "type": "ACTUATOR", "name": "Onboard_LED", "desc": "Indicator"}
        ],
        "auth_required": [70, 71]  # IOW, IOR
    }
```

### Skill B: execute_m_logic

```python
def execute_m_logic(m_tokens: list[int], description: str = "", timeout_ms: int = 5000) -> dict:
    """下发 M-Token 执行"""
    # 1. GATEWAY 授权
    if uses_io_instructions(m_tokens):
        vm.caps[70] = 1  # IOW
        vm.caps[71] = 1  # IOR
    
    # 2. 静态验证
    result = m_validate(m_tokens)
    if not result.valid:
        return {"error": result.msg}
    
    # 3. Varint 编码
    bytecode = encode_varint(m_tokens)
    
    # 4. 串口发送
    serial.write(len(bytecode).to_bytes(4, 'little'))
    serial.write(bytecode)
    
    # 5. 等待响应
    response = wait_for_response(timeout_ms)
    
    return parse_response(response)
```

### Skill C: read_vm_state

```python
def read_vm_state() -> dict:
    """读取 VM 状态"""
    return {
        "completed": True,
        "fault": "NONE",
        "fault_code": 0,
        "steps": 15,
        "sp": 0,
        "stack": [42],
        "result": 42
    }
```

## 辅助函数

```python
def m_validate(tokens: list[int]) -> ValidatorResult:
    """静态验证字节码"""
    # 检查 opcode 范围
    # 检查栈平衡
    # 检查跳转范围
    # 检查局部变量访问
    pass

def uses_io_instructions(tokens: list[int]) -> bool:
    """检查是否使用 IO 指令"""
    io_opcodes = {70, 71, 80}  # IOW, IOR, GTWAY
    return any(t in io_opcodes for t in tokens)
```

