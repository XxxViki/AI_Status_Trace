#!/usr/bin/env python3
"""Trae hook wrapper v5.7: read stdin, slim it, enqueue for watchdog.

Background:
  Claude Code hooks run through cmd.exe where `findstr /R ".*"` captures stdin cleanly.
  Trae hooks run through PowerShell → cmd.exe on Windows, and findstr MANGLES the JSON
  payload (observed: watchdog drops Trae events as "非JSON事件丢弃" because body is truncated).
  This script bypasses cmd/findstr entirely — sys.stdin.buffer.read() gives us clean bytes.

v5.7: SLIM THE BODY.
  Trae's PostToolUse stdin includes the FULL tool_response (28KB+ for Write/Edit).
  Sending that 28KB to ESP32 times out the watchdog's 2s POST → FIFO stalls,
  and every subsequent event gets stuck behind it.
  Fix: after reading stdin, parse JSON, keep only fields the board needs
  (session_id, cwd, tool_name, hook_event_name), discard everything else.
  Queue files drop from 28KB to ~100 bytes; watchdog forwards in <100ms.

Usage (invoked by Trae hooks.json):
  python -X utf8 trae_hook.py <event_type> trae
"""
import sys
import os
import random
import json
from datetime import datetime

def slim_body(raw: bytes) -> bytes:
    """Keep only fields the board needs; drop tool_response/tool_input/etc."""
    try:
        j = json.loads(raw.decode("utf-8", "replace"))
        slim = {}
        # session_id is REQUIRED — board routes by it
        for k in ("session_id", "sessionId"):
            v = j.get(k)
            if v:
                slim["session_id"] = v
                break
        # cwd / project path — shows as card project name
        for k in ("cwd",):
            v = j.get(k)
            if v:
                slim["cwd"] = v
                break
        # tool_name — shows as "last tool used" on card
        for k in ("tool_name", "toolName", "llm_tool_name"):
            v = j.get(k)
            if v:
                slim["tool_name"] = v
                break
        return json.dumps(slim).encode("utf-8")
    except Exception:
        return raw  # parse failed — keep original (better than nothing)


def main() -> int:
    if len(sys.argv) < 2:
        return 0

    event = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else "trae"

    log_dir = os.path.join(os.path.expanduser("~"), ".ai_status")
    os.makedirs(log_dir, exist_ok=True)

    ts = datetime.now().strftime("[%Y/%m/%d %H:%M:%S.%f")[:-3] + "]"

    # Log execution
    try:
        with open(os.path.join(log_dir, "hook_exec.log"), "a", encoding="utf-8") as f:
            f.write(f"{ts} ev={event} src={src} cwd={os.getcwd()}\n")
    except Exception:
        pass

    # Read stdin BINARY — avoid PowerShell encoding quirks
    try:
        raw_body = sys.stdin.buffer.read()
    except Exception:
        raw_body = b""

    # v5.7: SLIM IT — 28KB PostToolUse → ~100 bytes
    body = slim_body(raw_body)

    # DEBUG: dump to trae_stdin.log (raw + slim size) — keep for now
    try:
        with open(os.path.join(log_dir, "trae_stdin.log"), "a", encoding="utf-8") as f:
            f.write(f"{ts} ev={event} src={src} raw={len(raw_body)}B slim={len(body)}B\n")
            try:
                j = json.loads(body.decode("utf-8", "replace")) if body.strip() else {}
                f.write(f"  session_id={j.get('session_id','')} "
                        f"tool_name={j.get('tool_name','')} "
                        f"cwd={j.get('cwd','')}\n")
            except Exception:
                pass
    except Exception:
        pass

    # Write slim body to queue
    qdir = os.path.join(log_dir, "queue")
    os.makedirs(qdir, exist_ok=True)
    fname = f"{event}_{src}_{random.randint(0, 10**9)}.ev"
    try:
        with open(os.path.join(qdir, fname), "wb") as f:
            f.write(body)
    except Exception:
        pass

    return 0

if __name__ == "__main__":
    sys.exit(main())
