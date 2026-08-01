# M-Language Core

AI-native hardware control plane: Natural language → M-IR → M-Token bytecode → bounded safe execution. Includes MVM, MCP tool layer, and Skills orchestration.

## Features

- **MVM**: Stack-based virtual machine with 80+ instructions, gas metering, capability gating
- **M-IR Compiler**: LLM-generated intermediate language → deterministic M-Token bytecode
- **MCP Tool Layer**: 25+ tools for sensor reading, relay control, audit logging
- **Skills Orchestration**: 4 workflows (patrol, environment assessment, safety control, closed-loop relay)
- **ESP8266 Firmware**: Serial-based hardware execution
- **Safety**: Circuit breaker, audit trail, confirmation gate, sandbox

## Architecture

```
User (Natural Language)
    │
    ▼
LLM (Ollama)
    │ M-IR text
    ▼
M-IR Compiler (mlang/compiler)
    │ M-Token bytecode
    ▼
MVM Execution
    ├─ MCU: ESP8266 (serial)
    └─ PC: Local simulator → syscall
```

## Quick Start

```powershell
scripts\start_all.bat
```

Services:
- Ollama: http://127.0.0.1:11434
- Tool Router: http://127.0.0.1:8000/v1
- MCP Server: http://127.0.0.1:9001/mcp
- Open WebUI: http://127.0.0.1:8080

## Development

```powershell
# Run MCP server
cd python\mcp
python server.py

# Run tests
cd tests
python -m pytest test_lir_backend.py test_skills.py -v
```

## Documentation

- [docs/40_施工路线图.md](docs/40_施工路线图.md) — Current roadmap
- [docs/mcp和skills.md](docs/mcp和skills.md) — MCP & Skills specification
- [docs/M-Token规范.md](docs/M-Token规范.md) — M-Token specification

## License

MIT
