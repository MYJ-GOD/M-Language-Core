import asyncio
import json
import os
import signal
import sys
from typing import Any, Dict, List
from asyncio import timeout as asyncio_timeout

import httpx
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

import anyio

from mcp.client.session import ClientSession
from mcp.client.streamable_http import streamable_http_client

from contextlib import asynccontextmanager

# Configuration - Local Ollama
OLLAMA_BASE = os.getenv("OLLAMA_BASE_URL", "http://localhost:11434")
DEFAULT_MODEL = os.getenv("OLLAMA_MODEL", "qwen3:32b")
MAX_TOOL_ROUNDS = int(os.getenv("TOOL_MAX_ROUNDS", "4"))
MCP_CONNECT_TIMEOUT = float(os.getenv("MCP_CONNECT_TIMEOUT_SEC", "20"))
MCP_CONNECT_RETRIES = int(os.getenv("MCP_CONNECT_RETRIES", "3"))
MCP_CONNECT_RETRY_DELAY = float(os.getenv("MCP_CONNECT_RETRY_DELAY_SEC", "1.5"))

# MCP URL
raw_mcp_url = os.getenv("MCP_HTTP_URL", "http://127.0.0.1:9001/mcp")
MCP_HTTP_URL = raw_mcp_url.strip().rstrip("/")

# MCP session state
_mcp_session: ClientSession | None = None
_mcp_cm = None
_mcp_session_cm = None
_mcp_read = None
_mcp_write = None
_mcp_get_session_id = None
_mcp_lock = asyncio.Lock()
_mcp_init_lock = asyncio.Lock()
_mcp_connection_error: str | None = None


async def _ensure_mcp_session() -> ClientSession | None:
    """Ensure we have a valid MCP session, return None if unavailable."""
    global _mcp_session, _mcp_session_cm, _mcp_read, _mcp_write, _mcp_cm, _mcp_get_session_id, _mcp_connection_error
    
    if _mcp_session is not None:
        return _mcp_session
    
    async with _mcp_init_lock:
        if _mcp_session is not None:
            return _mcp_session

        last_error: str | None = None
        for attempt in range(1, max(1, MCP_CONNECT_RETRIES) + 1):
            try:
                print(
                    f"[MCP] Connecting to {MCP_HTTP_URL} "
                    f"(attempt {attempt}/{MCP_CONNECT_RETRIES}, timeout={MCP_CONNECT_TIMEOUT}s)...",
                    file=sys.stderr,
                )
                _mcp_cm = streamable_http_client(MCP_HTTP_URL)
                async with asyncio_timeout(MCP_CONNECT_TIMEOUT):
                    _mcp_read, _mcp_write, _mcp_get_session_id = await _mcp_cm.__aenter__()
                _mcp_session_cm = ClientSession(_mcp_read, _mcp_write)
                async with asyncio_timeout(MCP_CONNECT_TIMEOUT):
                    _mcp_session = await _mcp_session_cm.__aenter__()
                    await _mcp_session.initialize()
                print("[MCP] Connected successfully!", file=sys.stderr)
                _mcp_connection_error = None
                return _mcp_session
            except asyncio.TimeoutError:
                last_error = f"MCP connection timeout ({MCP_CONNECT_TIMEOUT}s)"
            except Exception as e:
                last_error = str(e)

            # Cleanup partial state before next retry.
            await _reset_mcp_session()
            _mcp_connection_error = last_error
            print(f"[MCP] Connection failed (attempt {attempt}): {last_error}", file=sys.stderr)
            if attempt < MCP_CONNECT_RETRIES:
                await asyncio.sleep(MCP_CONNECT_RETRY_DELAY)

        return None


async def _reset_mcp_session() -> None:
    """Reset MCP session state."""
    global _mcp_session, _mcp_session_cm, _mcp_read, _mcp_write, _mcp_cm, _mcp_get_session_id, _mcp_connection_error
    _mcp_session = None
    if _mcp_session_cm is not None:
        try:
            await _mcp_session_cm.__aexit__(None, None, None)
        except Exception:
            pass
    _mcp_session_cm = None
    _mcp_read = None
    _mcp_write = None
    _mcp_get_session_id = None
    _mcp_connection_error = None
    if _mcp_cm is not None:
        try:
            await _mcp_cm.__aexit__(None, None, None)
        except Exception:
            pass
    _mcp_cm = None


async def _mcp_tools() -> List[Dict[str, Any]]:
    """Get list of available MCP tools."""
    global _mcp_connection_error
    
    session = await _ensure_mcp_session()
    if session is None:
        if _mcp_connection_error:
            raise HTTPException(
                status_code=503,
                detail={
                    "message": f"MCP server unavailable: {_mcp_connection_error}",
                    "type": "mcp_unavailable",
                    "hint": "Make sure MCP server is running on " + MCP_HTTP_URL
                }
            )
        return []
    
    try:
        resp = await session.list_tools()
    except anyio.ClosedResourceError:
        await _reset_mcp_session()
        session = await _ensure_mcp_session()
        if session is None:
            return []
        resp = await session.list_tools()
    except Exception as e:
        print(f"[MCP] Error listing tools: {e}", file=sys.stderr)
        return []
    
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
    """Call an MCP tool."""
    global _mcp_connection_error
    
    session = await _ensure_mcp_session()
    if session is None:
        if _mcp_connection_error:
            raise HTTPException(
                status_code=503,
                detail={
                    "message": f"MCP server unavailable: {_mcp_connection_error}",
                    "type": "mcp_unavailable"
                }
            )
        raise HTTPException(status_code=503, detail="MCP server not connected")
    
    try:
        resp = await session.call_tool(name, args)
    except anyio.ClosedResourceError:
        await _reset_mcp_session()
        session = await _ensure_mcp_session()
        if session is None:
            raise HTTPException(status_code=503, detail="MCP server disconnected")
        resp = await session.call_tool(name, args)
    except Exception as e:
        print(f"[MCP] Error calling tool {name}: {e}", file=sys.stderr)
        raise HTTPException(status_code=500, detail=f"Tool call failed: {str(e)}")
    
    if hasattr(resp, "content"):
        return [c.model_dump() if hasattr(c, "model_dump") else c for c in resp.content]
    return resp


def _chat_url() -> str:
    """Get the chat completions URL for local Ollama."""
    base = (OLLAMA_BASE or "").rstrip("/")
    if base.endswith("/v1"):
        return f"{base}/chat/completions"
    return f"{base}/v1/chat/completions"


async def _ollama_chat(payload: Dict[str, Any]) -> Dict[str, Any]:
    """Send chat request to local Ollama."""
    async with httpx.AsyncClient() as client:
        resp = await client.post(_chat_url(), json=payload, timeout=300.0)
        try:
            data = resp.json()
        except Exception:
            raise HTTPException(
                status_code=resp.status_code,
                detail={
                    "message": resp.text,
                    "type": "upstream_non_json",
                    "upstream_status": resp.status_code,
                },
            )
        if resp.status_code != 200:
            raise HTTPException(status_code=resp.status_code, detail=data)
        return data


async def _ollama_models() -> List[str]:
    """Fetch available model names from local Ollama."""
    base = (OLLAMA_BASE or "").rstrip("/")
    url = f"{base}/api/tags"
    async with httpx.AsyncClient() as client:
        resp = await client.get(url, timeout=15.0)
        if resp.status_code != 200:
            raise HTTPException(
                status_code=resp.status_code,
                detail={
                    "message": resp.text,
                    "type": "upstream_model_list_failed",
                    "upstream_status": resp.status_code,
                },
            )
        data = resp.json()
        models = data.get("models", []) if isinstance(data, dict) else []
        names: List[str] = []
        for model in models:
            if not isinstance(model, dict):
                continue
            name = model.get("name")
            if isinstance(name, str) and name.strip():
                names.append(name.strip())
        if not names:
            names = [DEFAULT_MODEL]
        return names


# FastAPI app setup with graceful shutdown
shutdown_event = asyncio.Event()

def signal_handler(signum, frame):
    print(f"\nReceived signal {signum}, initiating graceful shutdown...", file=sys.stderr)
    shutdown_event.set()

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

@asynccontextmanager
async def lifespan(app: FastAPI):
    print("[Router] Starting up...", file=sys.stderr)
    print(f"[Router] Ollama: {OLLAMA_BASE}, Model: {DEFAULT_MODEL}", file=sys.stderr)
    yield
    print("[Router] Shutting down gracefully...", file=sys.stderr)
    await _reset_mcp_session()
    shutdown_event.set()

app = FastAPI(lifespan=lifespan)


@app.exception_handler(Exception)
async def _unhandled_exception_handler(_req: Request, exc: Exception):
    return JSONResponse(
        status_code=500,
        content={"error": {"message": str(exc), "type": "unhandled_exception"}},
    )


@app.get("/health")
async def health() -> Dict[str, Any]:
    """Health check endpoint."""
    session = await _ensure_mcp_session()
    return {
        "status": "ok",
        "mcp_connected": session is not None,
        "mcp_error": _mcp_connection_error if session is None else None
    }


@app.get("/v1/models")
async def list_models() -> Dict[str, Any]:
    model_names = await _ollama_models()
    return {
        "object": "list",
        "data": [{"id": name, "object": "model"} for name in model_names],
    }


@app.post("/v1/chat/completions")
async def chat_completions(req: Request) -> Dict[str, Any]:
    print(f"[Chat] Received request", file=sys.stderr)
    
    body = await req.json()
    print(f"[Chat] Body: {json.dumps(body, ensure_ascii=False)[:500]}...", file=sys.stderr)
    
    # Disable streaming for now
    if body.get("stream"):
        body["stream"] = False
        print("[Chat] Disabled streaming", file=sys.stderr)

    messages = body.get("messages", [])
    if not isinstance(messages, list):
        raise HTTPException(status_code=400, detail="messages must be a list")

    model = body.get("model", DEFAULT_MODEL)
    print(f"[Chat] Model: {model}", file=sys.stderr)
    
    tools = await _mcp_tools()
    print(f"[Chat] Got {len(tools)} tools from MCP", file=sys.stderr)

    # Build payload for Ollama (OpenAI compatible format)
    payload = {
        "model": model,
        "messages": messages,
    }
    
    if tools:
        payload["tools"] = tools
        payload["tool_choice"] = body.get("tool_choice", "auto")

    print(f"[Chat] Sending to Ollama...", file=sys.stderr)
    try:
        last = await _ollama_chat(payload)
        print(f"[Chat] Got response: {json.dumps(last, ensure_ascii=False)[:300]}...", file=sys.stderr)
    except Exception as e:
        print(f"[Chat] Error from Ollama: {e}", file=sys.stderr)
        raise
    
    print(f"[Chat] Got response, choices: {len(last.get('choices', []))}", file=sys.stderr)

    for i in range(MAX_TOOL_ROUNDS):
        choices = last.get("choices", [])
        if not choices:
            print(f"[Chat] No choices, returning", file=sys.stderr)
            return last

        msg = choices[0].get("message", {})
        content = msg.get("content", "")
        tool_calls = msg.get("tool_calls", [])

        if content:
            print(f"[Chat] Round {i}: Has content, returning", file=sys.stderr)
            if tool_calls:
                return last
            else:
                return last

        if not tool_calls:
            print(f"[Chat] Round {i}: No tool calls, returning", file=sys.stderr)
            return last

        print(f"[Chat] Round {i}: {len(tool_calls)} tool calls", file=sys.stderr)
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

                print(f"[Chat] Calling tool: {name}", file=sys.stderr)
                result = await _call_tool(name, args)
                print(f"[Chat] Tool result: {str(result)[:200]}...", file=sys.stderr)
                
                if isinstance(result, list):
                    result_text = ""
                    for item in result:
                        if isinstance(item, dict) and "text" in item:
                            result_text += item["text"]
                        elif isinstance(item, str):
                            result_text += item
                        else:
                            result_text += json.dumps(item, ensure_ascii=False)
                    result_text = result_text.strip()
                elif isinstance(result, dict):
                    result_text = json.dumps(result, ensure_ascii=False, indent=2)
                else:
                    result_text = str(result)
                messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", ""),
                    "content": result_text
                })

        payload["messages"] = messages
        last = await _ollama_chat(payload)

    print(f"[Chat] Max rounds reached, returning", file=sys.stderr)
    return last
