#!/usr/bin/env python3
"""
AI Status 主机侧看门狗（双职责）

背景：
  #027 hook 是事件驱动的，进程被杀不产生事件 -> 需要主动巡检进程表（本脚本职责一）
  #028 ZCode 的 7 个 hook 事件里没有"批准"——批准后要等工具跑完(PostToolUse)
       卡片才回 WORKING；而 ZCode 自己的日志里记着 tool.permission.resolved
       的确切时刻 + sessionId -> 尾随日志，批准即推 WORKING（本脚本职责二）

主循环：
  每 1 秒: 尾随 ZCode 日志，发现 permission.resolved -> 往板子推 WORKING
  每 3 秒: 巡检进程表，清理"进程已死但卡片还在"的残留

日志：~/.ai_status/watchdog.log（只在动作和异常时写）
"""
import glob
import json
import os
import subprocess
import sys
import time
import urllib.request

BOARD = "http://192.168.1.20"
LOG_PATH = os.path.join(os.path.expanduser("~"), ".ai_status", "watchdog.log")
ZCODE_LOG_GLOB = os.path.join(os.path.expanduser("~"), ".zcode", "cli", "log", "zcode-*.jsonl")
PROC_SCAN_EVERY = 3   # 秒


def log(msg: str) -> None:
    try:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}\n")
    except Exception:
        pass


def opener():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def board_state():
    with opener().open(BOARD + "/state", timeout=4) as r:
        return json.loads(r.read())


def board_clear(tool=None, id_prefix=None):
    url = BOARD + "/sessions/clear?"
    if tool:
        url += f"tool={tool}&"
    if id_prefix:
        url += f"id={id_prefix}&"
    req = urllib.request.Request(url, data=b"{}",
                                 headers={"Content-Type": "application/json"})
    with opener().open(req, timeout=4) as r:
        return json.loads(r.read())


def board_event(event_type, src, session_id, tool_name=None):
    import urllib.parse
    url = f"{BOARD}/events?event_type={event_type}&src={src}"
    body = {"session_id": session_id}
    if tool_name:
        body["tool_name"] = tool_name
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with opener().open(req, timeout=4) as r:
        return r.read().decode()


def process_names():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                         capture_output=True, timeout=10,
                         creationflags=0x08000000)   # CREATE_NO_WINDOW
    names = []
    for line in out.stdout.decode("gbk", "replace").splitlines():
        line = line.strip()
        if line.startswith('"'):
            names.append(line.split('","')[0].strip('"').lower())
    return names


class ZCodeLogTailer:
    """尾随 ZCode 日志，捕捉 tool.permission.resolved（= 用户点了批准/拒绝）"""

    def __init__(self, path_glob):
        self.path_glob = path_glob
        self.path = None
        self.offset = 0
        self.pending = b""

    def _current_file(self):
        files = glob.glob(self.path_glob)
        return max(files, key=os.path.getmtime) if files else None

    def poll(self):
        """返回本轮新增的 resolved 记录列表 [{session, decision, tool}]"""
        found = []
        cur = self._current_file()
        if cur is None:
            return found
        if cur != self.path:
            # 新文件（启动或跨天轮转）：只跟增量，不动历史
            self.path = cur
            try:
                self.offset = os.path.getsize(cur)
            except OSError:
                self.offset = 0
            self.pending = b""
            return found
        try:
            size = os.path.getsize(cur)
            if size < self.offset:      # 文件被截断/重写
                self.offset = 0
                self.pending = b""
            if size == self.offset:
                return found
            with open(cur, "rb") as f:
                f.seek(self.offset)
                chunk = f.read()
                self.offset = f.tell()
        except OSError:
            return found

        data = self.pending + chunk
        *lines, self.pending = data.split(b"\n")
        for raw in lines:
            if b'"tool.permission.resolved"' not in raw:
                continue
            try:
                rec = json.loads(raw.decode("utf-8", "replace"))
                ctx = rec.get("context", {})
                found.append({
                    "session": rec.get("sessionId", ""),
                    "decision": ctx.get("decision", ""),
                    "tool": ctx.get("toolName", ""),
                })
            except Exception:
                pass
        return found


def proc_rules():
    """职责一：进程被杀 -> 清残留卡（原始规则见问题记录 #027）"""
    names = process_names()
    claude_n = sum(1 for n in names if n == "claude.exe")
    zcode_alive = any(n == "zcode.exe" for n in names)

    st = board_state()
    cards = st.get("table", [])
    claude_cards = [c for c in cards if c.get("tool") == "claude"]
    zcode_cards = [c for c in cards if c.get("tool") == "zcode"]

    # 防护：只清"安静至少 10 秒"的卡（进程名检测若失效也不误杀活跃会话）
    quiet = [c for c in claude_cards if c.get("idle_s", 0) >= 10]
    if quiet and claude_n == 0 and len(quiet) == len(claude_cards):
        r = board_clear(tool="claude")
        log(f"claude 进程 0 个但板上 {len(claude_cards)} 卡 -> 全清 {r}")
    elif claude_cards and 0 < claude_n < len(claude_cards):
        victims = sorted(quiet, key=lambda c: -c.get("idle_s", 0))
        victims = victims[: max(0, len(claude_cards) - claude_n)]
        for v in victims:
            r = board_clear(id_prefix=v["id"])
            log(f"claude 进程 {claude_n} < 卡数 {len(claude_cards)} "
                f"-> 清最闲 {v['id']}(idle {v.get('idle_s')}s) {r}")

    if zcode_cards and not zcode_alive:
        r = board_clear(tool="zcode")
        log(f"ZCode 应用未运行但板上 {len(zcode_cards)} 卡 -> 全清 {r}")


def main() -> int:
    log("看门狗启动(v2: 进程巡检 + 批准加速)")
    tailer = ZCodeLogTailer(ZCODE_LOG_GLOB)
    tick = 0
    while True:
        tick += 1
        # 职责二：批准/拒绝 -> 立即推 WORKING（每 1 秒）
        try:
            for rec in tailer.poll():
                if rec["session"]:
                    board_event("pre-tool-use", "zcode", rec["session"], rec["tool"])
                    log(f"批准/拒绝已解决({rec['decision']} {rec['tool']}) "
                        f"-> 推 WORKING {rec['session'][:12]}")
        except Exception as e:
            log(f"日志尾随异常: {type(e).__name__}: {e}")

        # 职责一：进程巡检（每 3 秒）
        if tick % PROC_SCAN_EVERY == 0:
            try:
                proc_rules()
            except Exception as e:
                log(f"进程巡检跳过: {type(e).__name__}: {e}")

        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
