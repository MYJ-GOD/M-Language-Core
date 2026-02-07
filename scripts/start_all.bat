@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

REM ================================================
REM M-Language Start (Local, No Docker)
REM ================================================

set ROOT=C:\Users\34571\Desktop\M-Language-Core
set PY=D:\miniconda3\envs\mlang-mcp\python.exe
set OLLAMA_EXE=D:\Ollama\ollama.exe

REM Serial config
set MCP_SERIAL_PORT=COM3
set MCP_BAUD=115200

REM Local Ollama config for Tool Router
set OLLAMA_BASE_URL=http://127.0.0.1:11434
set OLLAMA_MODEL=qwen3:32b
set MCP_HTTP_URL=http://127.0.0.1:9001/mcp

REM Open WebUI connects to local Tool Router
set OPENAI_API_BASE_URL=http://127.0.0.1:8000/v1
set OPENAI_API_KEY=dummy
set ENABLE_OLLAMA_API=false

echo ================================================
echo   M-Language Start

echo ================================================
echo.

REM 1) Ensure Ollama is running
echo [1/4] Ensuring Ollama is running...
%OLLAMA_EXE% ps >nul 2>nul
if errorlevel 1 (
  start "Ollama" cmd /k "%OLLAMA_EXE% serve"
  echo      [OK] Ollama starting (11434)
  timeout /t 3 /nobreak >nul
) else (
  echo      [OK] Ollama already running (11434)
)

echo.

REM 2) Start MCP Server (serial bridge)
echo [2/4] Starting MCP Server...
start "MCP Server" cmd /k "set MCP_SERIAL_PORT=%MCP_SERIAL_PORT% && set MCP_BAUD=%MCP_BAUD% && %PY% %ROOT%\\python\\MCP\\server.py"

echo      [OK] MCP Server starting (9001)
set MCP_READY=0
for /L %%i in (1,1,30) do (
  powershell -NoProfile -Command "$c=New-Object Net.Sockets.TcpClient; try { $c.Connect('127.0.0.1',9001); exit 0 } catch { exit 1 } finally { $c.Close() }" >nul 2>nul
  if not errorlevel 1 (
    set MCP_READY=1
    goto :mcp_ready
  )
  timeout /t 1 /nobreak >nul
)
:mcp_ready
if "!MCP_READY!"=="1" (
  echo      [OK] MCP Server port is ready
) else (
  echo      [WARN] MCP Server port not ready within 30s, continuing...
)

echo.

REM 3) Start Tool Router (connects to local Ollama + MCP)
echo [3/4] Starting Tool Router...
start "Tool Router" cmd /k "set OLLAMA_BASE_URL=%OLLAMA_BASE_URL% && set OLLAMA_MODEL=%OLLAMA_MODEL% && set MCP_HTTP_URL=%MCP_HTTP_URL% && %PY% -m uvicorn router:app --app-dir %ROOT%\\tool-router --host 127.0.0.1 --port 8000"

echo      [OK] Tool Router starting (8000)
timeout /t 2 /nobreak >nul

echo.

REM 4) Start Open WebUI (connects to Tool Router)
echo [4/4] Starting Open WebUI...
start "Open WebUI" cmd /k "set ENABLE_OLLAMA_API=%ENABLE_OLLAMA_API% && set OPENAI_API_BASE_URL=%OPENAI_API_BASE_URL% && set OPENAI_API_KEY=%OPENAI_API_KEY% && D:\miniconda3\envs\mlang-mcp\Scripts\open-webui.exe serve --host 127.0.0.1 --port 8080"

echo      [OK] Open WebUI starting (8080)
timeout /t 3 /nobreak >nul

echo.

REM 5) Done
echo [5/5] Ready

echo.
echo ================================================
echo   Startup Complete

echo ================================================
echo.
echo   Service URLs:
echo     - Ollama:      http://127.0.0.1:11434
echo     - Tool Router: http://127.0.0.1:8000/v1
echo     - Open WebUI:  http://127.0.0.1:8080
echo     - MCP Server:  http://127.0.0.1:9001
echo.
echo   Open WebUI connection:
echo     - Base URL: http://127.0.0.1:8000/v1
echo     - API Key:  dummy
echo     - Model:    qwen3:32b
echo.
echo ================================================
echo.
pause
endlocal

