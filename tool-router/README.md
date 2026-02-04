# Tool Router (Ollama + MCP Skills)

此服务提供 OpenAI-compatible 的 `/v1/chat/completions` 接口，并将模型的 tool call 转发到本地 MCP skills。

## 运行前准备

- 安装依赖：`tool-router/requirements.txt`
- 设置环境变量（可选）：
  - `OLLAMA_BASE_URL`，默认 `http://localhost:11434`
  - `OLLAMA_MODEL`，默认 `deepseekr1-8b`
  - `TOOL_MAX_ROUNDS`，默认 `4`
  - `MCP_SERVER_PATH`，默认 `MCP/server.py`
  - `MCP_SERIAL_PORT`，串口设备路径
  - `MCP_BAUD`，默认 `115200`

## 启动

使用任意 ASGI 方式启动 `tool-router/router.py`。
