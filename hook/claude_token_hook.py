#!/usr/bin/env python3
"""
Claude Stop 专用 hook：转发事件 + 附带 token 用量

数据链：stdin payload(transcript_path) -> 读 transcript 尾部 256KB -> 逐行解析
JSON，取"最后一个非零 usage"的 input+cache 总量（= Claude 界面 ↓ Nk tokens）
-> 写入本地队列 ~/.ai_status/queue/stop_claude_<rand>[_t<tok>].ev（body 原样）
-> 常驻看门狗转发到板子（#030：网络移出 AI 关键路径）

设计约束：
  - 永远 exit 0（hook 不许报错）
  - token 提取尽力而为：任何失败 -> 不带 tok 照常投递
  - 只挂在 Stop 事件（每轮一次，Python 冷启动 ~300ms 可接受）
"""
import json
import os
import random
import sys

TAIL_BYTES = 256 * 1024
LOG_PATH = os.path.join(os.path.expanduser("~"), ".ai_status", "tok_hook.log")
QUEUE_DIR = os.path.join(os.path.expanduser("~"), ".ai_status", "queue")


def dbg(msg: str) -> None:
    try:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(msg + "\n")
    except Exception:
        pass


def extract_tokens(transcript_path: str) -> int:
    """返回最后一个非零 usage 的 input+cache+creation 总量；找不到返回 0"""
    if not transcript_path or not os.path.exists(transcript_path):
        return 0
    with open(transcript_path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - TAIL_BYTES))
        tail = f.read().decode("utf-8", "replace")

    total = 0
    for line in tail.splitlines():
        line = line.strip()
        if not line or not line.startswith("{"):
            continue        # 尾部首行可能是半行，跳过
        try:
            obj = json.loads(line)
        except Exception:
            continue
        # usage 可能在顶层或 message 下（不同记录类型）
        msg = obj.get("message")
        u = msg.get("usage") if isinstance(msg, dict) else None
        if not isinstance(u, dict):
            u = obj.get("usage")
        if not isinstance(u, dict):
            continue
        t = (int(u.get("input_tokens") or 0)
             + int(u.get("cache_read_input_tokens") or 0)
             + int(u.get("cache_creation_input_tokens") or 0))
        if t > 0:
            total = t     # 只看最后一个非零（文件按时间追加）
    return total


def main() -> int:
    raw = sys.stdin.read()
    tok = 0
    tp = ""
    sid = ""
    try:
        payload = json.loads(raw) if raw.strip() else {}
        tp = payload.get("transcript_path", "") or ""
        sid = payload.get("session_id", "") or ""
        tok = extract_tokens(tp)
    except Exception as e:
        dbg(f"extract failed: {type(e).__name__}: {e}")

    # 投递到本地队列（#030）：网络由常驻看门狗转发，hook 零网络阻塞
    try:
        os.makedirs(QUEUE_DIR, exist_ok=True)
        suffix = f"_t{tok}" if tok > 0 else ""
        name = f"stop_claude_{random.randint(0, 10**9)}{suffix}.ev"
        with open(os.path.join(QUEUE_DIR, name), "wb") as f:
            f.write(raw.encode("utf-8", "replace"))
        dbg(f"queued: sid={sid[:12]} tok={tok} tpath={'ok' if tp else 'none'}")
    except Exception as e:
        dbg(f"queue failed: sid={sid[:12]} tok={tok} {type(e).__name__}: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
