import asyncio
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List

import httpx
from fastapi import FastAPI, HTTPException, Request

from mcp.client import ClientSession
from mcp.client.stdio import stdio_client

app = FastAPI()

OLLAMA_BASE = os.getenv("OLLAMA_BASE_URL", "http://localhost:11434")
DEFAULT_MODEL = os.getenv("OLLAMA_MODEL", "deepseekr1-8b")
MAX_TOOL_ROUNDS = int(os.getenv("TOOL_MAX_ROUNDS", "4"))
MCP_SERVER = os.getenv("MCP_SERVER_PATH", str(Path(__file__).resolve().parents[1] / "MCP" / "server.py"))

_mcp_session: ClientSession | None = None
_mcp_read = None
_mcp_write = None
_mcp_lock = asyncio.Lock()


async def _ensure_mcp_session() -> ClientSession:
    global _mcp_session, _mcp_read, _mcp_write
    if _mcp_session is not None:
        return _mcp_session
    cmd = [sys.executable, MCP_SERVER]
    _mcp_read, _mcp_write = await stdio_client(cmd)
    _mcp_session = ClientSession(_mcp_read, _mcp_write)
    await _mcp_session.initialize()
    return _mcp_session


async def _mcp_tools() -> List[Dict[str, Any]]:
    session = await _ensure_mcp_session()
    resp = await session.list_tools()
    tools = []
    for t in resp.tools:
        tools.append({
            "type": "function",
            "function": {
                "name": t.name,
                "description": t.description or "",
                "parameters": t.inputSchema or {"type": "object", "properties": {}}
            }
        })
    return tools


async def _call_tool(name: str, args: Dict[str, Any]) -> Any:
    session = await _ensure_mcp_session()
    resp = await session.call_tool(name, args)
    # MCP returns content list; pass through as text/json
    if hasattr(resp, "content"):
        return [c.model_dump() if hasattr(c, "model_dump") else c for c in resp.content]
    return resp


async def _ollama_chat(payload: Dict[str, Any]) -> Dict[str, Any]:
    async with httpx.AsyncClient() as client:
        resp = await client.post(f"{OLLAMA_BASE}/v1/chat/completions", json=payload, timeout=120.0)
        if resp.status_code != 200:
            raise HTTPException(status_code=resp.status_code, detail=resp.text)
        return resp.json()


@app.get("/health")
async def health() -> Dict[str, str]:
    return {"status": "ok"}


@app.post("/v1/chat/completions")
async def chat_completions(req: Request) -> Dict[str, Any]:
    body = await req.json()
    if body.get("stream"):
        raise HTTPException(status_code=400, detail="streaming not supported")

    messages = body.get("messages", [])
    if not isinstance(messages, list):
        raise HTTPException(status_code=400, detail="messages must be a list")

    model = body.get("model", DEFAULT_MODEL)
    tools = await _mcp_tools()

    payload = {
        "model": model,
        "messages": messages,
        "tools": tools,
        "tool_choice": body.get("tool_choice", "auto")
    }

    last = await _ollama_chat(payload)

    for _ in range(MAX_TOOL_ROUNDS):
        choices = last.get("choices", [])
        if not choices:
            return last

        msg = choices[0].get("message", {})
        tool_calls = msg.get("tool_calls", [])
        if not tool_calls:
            return last

        messages.append(msg)

        async with _mcp_lock:
            for tc in tool_calls:
                fn = tc.get("function", {})
                name = fn.get("name", "")
                args_raw = fn.get("arguments", "{}")
                try:
                    args = json.loads(args_raw) if isinstance(args_raw, str) else args_raw
                except Exception:
                    args = {}

                result = await _call_tool(name, args)
                messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", ""),
                    "content": json.dumps(result, ensure_ascii=False)
                })

        payload["messages"] = messages
        last = await _ollama_chat(payload)

    return last
