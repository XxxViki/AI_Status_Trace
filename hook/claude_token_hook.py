#!/usr/bin/env python3
"""
Claude Stop 专用 hook：转发事件 + 附带 token 用量

数据链：stdin payload(transcript_path) -> 读 transcript 尾部 -> 取最后一次
usage 的 input+cache tokens(即 Claude 界面的 "↓ Nk tokens" 上下文用量)
-> POST /events?event_type=stop&src=claude&tok=N (body 原样透传)

设计约束：
  - 永远 exit 0（hook 不许报错）
  - token 提取尽力而为：文件读不到/解析失败 -> 不带 tok 参数照样发事件
  - 只挂在 Stop 事件上（每轮一次，Python 冷启动 ~300ms 可接受）
"""
import json
import re
import sys
import urllib.request

BOARD = "http://192.168.1.20"

def main() -> int:
    raw = sys.stdin.read()
    tok = ""
    try:
        payload = json.loads(raw) if raw.strip() else {}
        tp = payload.get("transcript_path", "")
        if tp:
            with open(tp, "rb") as f:
                f.seek(0, 2)
                size = f.tell()
                f.seek(max(0, size - 256 * 1024))      # 只看尾部 256KB
                tail = f.read().decode("utf-8", "replace")
            # 最后一个 usage 块: {"input_tokens":N,...,"cache_read_input_tokens":N,...}
            blocks = re.findall(r'"input_tokens"\s*:\s*(\d+)', tail)
            cache = re.findall(r'"cache_read_input_tokens"\s*:\s*(\d+)', tail)
            cache_c = re.findall(r'"cache_creation_input_tokens"\s*:\s*(\d+)', tail)
            if blocks:
                tok = str(int(blocks[-1])
                          + (int(cache[-1]) if cache else 0)
                          + (int(cache_c[-1]) if cache_c else 0))
    except Exception:
        tok = ""   # 提取失败不影响事件转发

    url = f"{BOARD}/events?event_type=stop&src=claude"
    if tok:
        url += f"&tok={tok}"
    # 排障日志(#026): 记录归属会话与token,抓"token串门"
    try:
        sid_dbg = ""
        import re as _re
        m = _re.search(r'"session_id"\s*:\s*"([^"]+)"', raw)
        if m:
            sid_dbg = m.group(1)[:12]
        with open(__import__("os").path.expanduser("~/.ai_status/tok_hook.log"), "a") as f:
            f.write(f"sid={sid_dbg} tok={tok} tpath_exists={bool(tp)}
")
    except Exception:
        pass
    try:
        req = urllib.request.Request(url, data=raw.encode(),
                                     headers={"Content-Type": "application/json"})
        # 绕过系统代理 + 2 秒预算
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        opener.open(req, timeout=2).read()
    except Exception:
        pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
