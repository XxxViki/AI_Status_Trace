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
import random
import subprocess
import sys
import time
import urllib.request

BOARD = "http://192.168.1.20"
QUEUE_DIR = os.path.join(os.path.expanduser("~"), ".ai_status", "queue")
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


def board_event_raw(event_type, src, body: bytes, tok: int = 0):
    """原样转发一条队列事件（body 是 hook 的原始 stdin payload）。
    超时 2s：本机 2.4G 环境往返 0.3-2.7s，2s 是"够快"与"够宽容"的平衡点；
    失败由调用方保序重试（下轮），代价低。
    #033：Python 侧完整解析 body 提取 session/tool 放 URL——板内只扫前512字节，
    Stop 类大载荷的 sessionId 可能在截断线之后（会错落进 "?" 会话）。"""
    url = f"{BOARD}/events?event_type={event_type}&src={src}"
    try:
        j = json.loads(body.decode("utf-8", "replace")) if body.strip() else {}
        sid = j.get("session_id") or j.get("sessionId") or ""
        tool = j.get("tool_name") or j.get("toolName") or ""
        if sid:
            url += f"&sid={sid}"
        if tool:
            url += f"&tool={tool}"
    except Exception:
        pass
    if not sid:
        # 捉现行(#053): 没有 session id 的事件会落进板子的 "?" 桶(幽灵卡)
        log(f"无sid事件: ev={event_type} src={src} body[:90]={body[:90]!r}")
    if tok > 0:
        url += f"&tok={tok}"
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"})
    with opener().open(req, timeout=2) as r:
        return r.read().decode()


def enqueue_event(event_type, src, body: bytes, tok: int = 0):
    """把一条事件写入本地队列（与 hook wrapper 相同的入队格式），
    由 drain_queue 统一转发——所有投递共享重试与保序（#031）"""
    os.makedirs(QUEUE_DIR, exist_ok=True)
    suffix = f"_t{tok}" if tok > 0 else ""
    name = f"{event_type}_{src}_{random.randint(0, 10**9)}{suffix}.ev"
    with open(os.path.join(QUEUE_DIR, name), "wb") as f:
        f.write(body)


CLAUDE_PROJ_GLOB = os.path.join(os.path.expanduser("~"), ".claude", "projects", "*", "*.jsonl")
_tok_state = {}   # path -> [offset, last_tok, pending_bytes, recreate_at]
_claude_recreate_at = 0.0   # #054: claude 卡重建限流


def claude_token_watch():
    """职责四(#043)：实时 token 监视——直接跟 Claude 转录文件。

    用户要求"实时刷新"（不是每轮结束才变）。转录按行追加，每条 assistant
    消息带 usage；本函数增量读取新增行，取最新非零 usage 的总和，
    变化即推 POST /sessions/tok（只改 token 不改状态）。
    只跟 10 分钟内活跃的文件；pending 缓冲处理"写字过程中"的半行。"""
    import glob as _glob
    now = time.time()
    for path in _glob.glob(CLAUDE_PROJ_GLOB):
        try:
            if now - os.path.getmtime(path) > 600:
                continue
            size = os.path.getsize(path)
        except OSError:
            continue
        sid = os.path.basename(path)[:-6]          # 去 .jsonl
        st = _tok_state.get(path)
        if st is None:
            # [偏移, 已推送值, 半行缓冲, 上次补发重建时间]
            st = [max(0, size - 65536), 0, b"", 0.0, 0]   # [偏移,已推送,半行,重建时间,已知值]
            _tok_state[path] = st
        if size < st[0]:                           # 文件被重写/截断
            st[0] = 0
            st[2] = b""
        if size == st[0]:
            continue
        try:
            with open(path, "rb") as f:
                f.seek(st[0])
                chunk = f.read()
        except OSError:
            continue
        data = st[2] + chunk
        *lines, st[2] = data.split(b"\n")
        st[0] = size
        tok = st[4]                          # 已知最新值(独立于"已推送值")
        for raw in lines:
            raw = raw.strip()
            if not raw.startswith(b"{"):
                continue
            try:
                obj = json.loads(raw.decode("utf-8", "replace"))
            except Exception:
                continue
            m = obj.get("message")
            u = m.get("usage") if isinstance(m, dict) else None
            if not isinstance(u, dict):
                u = obj.get("usage")
            if isinstance(u, dict):
                t = (int(u.get("input_tokens") or 0)
                     + int(u.get("cache_read_input_tokens") or 0)
                     + int(u.get("cache_creation_input_tokens") or 0))
                if t > 0:
                    tok = t                          # 保留最后一个非零
        if tok > 0:
            st[4] = tok
        if st[4] > 0 and st[4] != st[1]:     # known != pushed -> 推送(失败保持-1下轮重试)
            tok = st[4]
            try:
                url = f"{BOARD}/sessions/tok?sid={sid}&tok={tok}"
                req = urllib.request.Request(url, data=b"{}",
                                             headers={"Content-Type": "application/json"})
                with opener().open(req, timeout=2) as r:
                    resp = json.loads(r.read())
                if resp.get("updated"):
                    st[1] = tok    # 推送成功:记录已推送值
                else:
                    st[1] = -1     # 板上暂无此卡(未建立/已被清):下次强制重推
                    # #054: 转录文件活跃说明会话活着——板上却没有卡(事件丢失或被误清)
                    # → 补发 session-start 重建, 限流 60s 防刷屏
                    now2 = time.time()
                    if now2 - st[3] > 60:
                        st[3] = now2
                        enqueue_event("session-start", "claude",
                                      json.dumps({"session_id": sid}).encode())
                        log(f"板上无卡, 补发 session-start 重建 {sid[:12]}")
            except Exception:
                pass
    # 状态表清理：防长跑后无限增长
    if len(_tok_state) > 32:
        for p in list(_tok_state):
            try:
                if now - os.path.getmtime(p) > 3600:
                    del _tok_state[p]
            except OSError:
                del _tok_state[p]


def drain_queue():
    """职责三(#030)：转发 hook 写下的本地事件队列到板子。

    队列条目 = 文件名编码元数据 + 文件内容为原始 body：
        <event>_<src>_<rand>[_t<tok>].ev
    严格 FIFO：按 mtime 顺序发送；成功即删；4xx 毒条目立即删；
    **首次网络失败立即停止本轮**（保序 + 防一轮被大量重试拖死），下轮从头重试；
    条目过旧(>180s)自动放弃（状态事件重放无意义）。
    """
    import glob as _glob
    files = _glob.glob(os.path.join(QUEUE_DIR, "*.ev"))
    if not files:
        return
    try:
        files.sort(key=lambda p: os.stat(p).st_mtime_ns)
    except OSError:
        pass
    now = time.time()
    for path in files[:30]:                     # 每轮上限，防突发洪峰
        name = os.path.basename(path)
        try:
            if now - os.path.getmtime(path) > 180:
                os.remove(path)                 # 太旧：状态事件重放无意义
                log(f"队列条目过旧丢弃: {name}")
                continue
            parts = name[:-3].split("_")        # 去 .ev
            if len(parts) < 3:
                os.remove(path)
                continue
            ev, src = parts[0], parts[1]
            tok = 0
            for p in parts[2:]:
                if p.startswith("t") and p[1:].isdigit():
                    tok = int(p[1:])
            with open(path, "rb") as f:
                body = f.read()
            board_event_raw(ev, src, body, tok)
            os.remove(path)
        except urllib.error.HTTPError as e:
            if 400 <= e.code < 500:
                os.remove(path)                 # 毒条目（板子拒收），删掉不重试
                log(f"队列条目被拒({e.code})丢弃: {name}")
            else:
                log(f"队列发送失败({e.code})，本轮停止保序: {name}")
                break                           # 5xx：网络/板子问题，停本轮
        except Exception as e:
            log(f"队列发送失败，本轮停止保序: {name} {type(e).__name__}")
            break                               # FIFO 不破坏 + 本轮不被拖死


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

    # #054: Claude 进程在跑但板上没有 claude 卡（事件丢失/被误清）→ 用最近活跃的
    # 转录文件重建卡（文件名即权威会话 id）。只重建不删除，安全。限流 60s。
    global _claude_recreate_at
    if claude_n > 0 and not claude_cards:
        now2 = time.time()
        if now2 - _claude_recreate_at > 60:
            _claude_recreate_at = now2
            # #059: 重建**所有**活跃转录(2小时内), 不只最新——用户可能同时开着
            # 多个 Claude 会话(如 Desktop 上的审批 + 项目里的工作)
            import glob as _g
            files = [f for f in _g.glob(CLAUDE_PROJ_GLOB)
                     if now2 - os.path.getmtime(f) < 21600]   # 6小时(#059)
            for f in files:
                sid = os.path.basename(f)[:-6]
                enqueue_event("session-start", "claude",
                              json.dumps({"session_id": sid}).encode())
                log(f"claude 进程在但无卡 -> 重建活跃会话 {sid[:12]}")

    # 防护：只清"安静至少 10 秒"的卡
    quiet = [c for c in claude_cards if c.get("idle_s", 0) >= 10]
    # 只在"进程全没了"时清——计数匹配(1<n)已被证实会清错卡（#044：
    # 幽灵"?"卡使卡数虚高，把真实会话卡当"最闲"误删），故只保留全清场景
    if quiet and claude_n == 0 and len(quiet) == len(claude_cards):
        r = board_clear(tool="claude")
        log(f"claude 进程 0 个但板上 {len(claude_cards)} 卡 -> 全清 {r}")

    if zcode_cards and not zcode_alive:
        r = board_clear(tool="zcode")
        log(f"ZCode 应用未运行但板上 {len(zcode_cards)} 卡 -> 全清 {r}")


def main() -> int:
    log("看门狗启动(v4: 队列转发 + 进程巡检 + 批准加速 + 实时token)")
    tailer = ZCodeLogTailer(ZCODE_LOG_GLOB)
    tick = 0
    while True:
        tick += 1
        # 职责四：实时 token 监视（每 1 秒，直接跟 Claude 转录文件 #043）
        try:
            claude_token_watch()
        except Exception as e:
            log(f"token监视异常: {type(e).__name__}: {e}")

        # 职责三：转发 hook 事件队列（每 1 秒，网络彻底移出 AI 关键路径 #030）
        try:
            drain_queue()
        except Exception as e:
            log(f"队列转发异常: {type(e).__name__}: {e}")

        # 职责二：批准/拒绝 -> 立即推 WORKING（每 1 秒）
        try:
            for rec in tailer.poll():
                if rec["session"]:
                    # 走队列而不是直连 POST：网络抖动时享受重试，
                    # 否则推送丢失会让卡片卡在 APPROVE（#031）
                    enqueue_event("pre-tool-use", "zcode", json.dumps({
                        "session_id": rec["session"],
                        "tool_name": rec["tool"],
                    }).encode())
                    log(f"批准/拒绝已解决({rec['decision']} {rec['tool']}) "
                        f"-> 入队 WORKING {rec['session'][:12]}")
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
