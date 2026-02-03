# M-Language MCP Server 协议

## 串口通信格式

### PC → ESP8266 (下行)

```
[4-byte LE length][varint bytecode...]
```

### ESP8266 → PC (上行)

```
[4-byte LE length][varint response...]
```

## 响应格式

### 正常完成 (HALT)

```
[result varint][steps varint][0x01 flag]
```

### 故障 (FAULT)

```
[fault_code varint][pc varint][0x00 flag]
```

## 指令字节码

见 `M-Token规范.md`

## 示例

```python
import serial

# 发送字节码
code = bytes([0x52])  # HALT
serial.write(len(code).to_bytes(4, 'little'))
serial.write(code)

# 接收响应
resp_len = int.from_bytes(serial.read(4), 'little')
resp = serial.read(resp_len)
```

