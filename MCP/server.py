# M-Language MCP Server 主程序

import asyncio
from mcp.server import Server
from mcp.server.stdio import stdio_server
import skills

app = Server("m-language-mcp")

# 注册 Skills
@app.list_tools()
async def list_tools():
    return [
        {
            "name": "get_hardware_topology",
            "description": "获取 ESP8266 设备清单，包括传感器和执行器",
            "inputSchema": {"type": "object", "properties": {}},
            "outputSchema": {
                "type": "object",
                "properties": {
                    "devices": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "id": {"type": "integer"},
                                "type": {"type": "string"},
                                "name": {"type": "string"},
                                "unit": {"type": "string"},
                                "desc": {"type": "string"}
                            }
                        }
                    }
                }
            }
        },
        {
            "name": "execute_m_logic",
            "description": "下发 M-Token 字节码到 ESP8266 执行",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "m_tokens": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "M-Token 整数数组"
                    },
                    "description": {
                        "type": "string",
                        "description": "自然语言描述"
                    }
                },
                "required": ["m_tokens"]
            }
        },
        {
            "name": "read_vm_state",
            "description": "读取 VM 执行状态和栈内容",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "stack_depth": {
                        "type": "integer",
                        "description": "要读取的栈深度"
                    }
                }
            }
        }
    ]

@app.call_tool()
async def call_tool(name: str, arguments: dict):
    if name == "get_hardware_topology":
        result = skills.get_hardware_topology()
        return [{"type": "text", "text": str(result)}]
    elif name == "execute_m_logic":
        result = skills.execute_m_logic(arguments["m_tokens"], arguments.get("description", ""))
        return [{"type": "text", "text": str(result)}]
    elif name == "read_vm_state":
        result = skills.read_vm_state()
        return [{"type": "text", "text": str(result)}]
    else:
        raise ValueError(f"Unknown tool: {name}")

async def main():
    async with stdio_server() as (read_stream, write_stream):
        await app.run(read_stream, write_stream, app.create_initialization_options())

if __name__ == "__main__":
    asyncio.run(main())

