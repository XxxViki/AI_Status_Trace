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
import socket
import subprocess
import sys
import threading
import time
import urllib.request

# #085: 板子地址不再写死（换网段即失联）——统一走 board_addr 解析：
# 环境变量 AI_STATUS_BOARD > ~/.ai_status/board_url > 家里默认。
# board_url() 按 mtime 缓存，改文件后下一轮巡检自动生效，看门狗无需重启。
from board_addr import board_url
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


# ---------- 失联自愈（#087）----------
# 换网络环境（手机配网/新路由/DHCP 换 IP）后板子地址变化，以前要人跑 find_board.py。
# 现在看门狗自己找回：失联 >15s 触发，先试 mDNS 域名（新固件注册 aistatus.local），
# 不通再按 MAC 扫本机 /24（复用 tools/find_board.py）。找回即改写 board_url——
# 配合 #085 的 mtime 缓存，下一轮巡检无缝切过去，队列里积压的事件接着转发。
# 退避 60s→180s→540s→900s 封顶：板子长期离线时不骚扰网络。
HEAL_AFTER_S = 15
HEAL_INTERVALS = [60, 180, 540, 900]
BOARD_MDNS = "aistatus.local"
_board_last_ok = time.time()
_disc_last_try = 0.0
_disc_step = 0
# 评审修复(#087): 发现在后台线程跑（扫网段最长 ~10s，同步跑会拖住 1s 主循环——
# 批准加速/实时token 全都停摆）。_disc_result=None 无待消费结果，否则 (url,) 元组
_disc_running = False
_disc_result = None


def _stamp_board_ok() -> None:
    global _board_last_ok
    _board_last_ok = time.time()


def _health_ok(url: str, timeout: float = 3) -> bool:
    try:
        with opener().open(url.rstrip("/") + "/health", timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


def _resolve_mdns(timeout: float = 2.5):
    """gethostbyname .local 在无应答网络可能阻塞数秒——线程里跑，限时回收"""
    box = {}

    def run():
        try:
            box["ip"] = socket.gethostbyname(BOARD_MDNS)
        except Exception:
            pass

    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(timeout)
    return box.get("ip")


def _discover_board_url():
    # 1) mDNS：板子新固件注册的域名。禁多播的网络解析失败/超时，走下一步。
    #    解析到过期缓存 IP 时 /health 会失败，自然落入 ARP 兜底
    ip = _resolve_mdns()
    if ip and _health_ok(f"http://{ip}"):
        return f"http://{BOARD_MDNS}"
    # 2) 本机 /24 ping 扫描填 ARP 表，按板子 MAC（烧录固定）反查 + /health 验明正身
    try:
        tools_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "tools")
        sys.path.insert(0, tools_dir)
        import find_board
        subs = find_board.local_subnets()
        if not subs:
            return None
        find_board.sweep(subs)
        for ip2, mac in find_board.arp_table().items():
            if mac.startswith(find_board.BOARD_MAC) and _health_ok(f"http://{ip2}"):
                return f"http://{ip2}"
    except Exception as e:
        log(f"自动发现异常: {type(e).__name__}: {e}")
    return None


def _apply_discovery(url) -> None:
    """消费一次发现结果：改写 board_url（保留注释行，只换生效行）"""
    global _disc_step
    now = time.time()
    if not url:
        _disc_step = min(_disc_step + 1, len(HEAL_INTERVALS) - 1)
        log(f"板子失联 {int(now - _board_last_ok)}s，自动发现未找到"
            f"（下次 {HEAL_INTERVALS[_disc_step]}s 后重试）")
        return
    _disc_step = 0
    _stamp_board_ok()
    old = board_url()
    if url == old:
        return
    try:
        tools_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "tools")
        sys.path.insert(0, tools_dir)
        import find_board
        find_board.write_board_url(url)   # 保留注释行，只换生效行
    except Exception as e:
        log(f"自愈写配置失败: {type(e).__name__}: {e}")
        return
    log(f"板子失联自愈: {old} -> {url}（自动改写 board_url）")


def auto_heal_tick() -> None:
    """每轮巡检调用：板子失联时后台线程找回地址（手机配网后 PC 侧零操作）。
    主循环只投递/消费结果，永不被 ~10s 的扫描阻塞（批准加速/实时token 不停摆）"""
    global _disc_last_try, _disc_running, _disc_result
    now = time.time()
    healthy = now - _board_last_ok < HEAL_AFTER_S
    if _disc_result is not None:     # 上一轮线程的产出，本轮消费
        url, _disc_result = _disc_result[0], None
        if not healthy:              # 板子已自己恢复则丢弃过期结果，不误改地址
            _apply_discovery(url)
        return
    if healthy:
        return                       # 板子活着，无需自愈
    if os.environ.get("AI_STATUS_BOARD"):
        return                       # 显式覆盖地址时不自作主张
    if _disc_running:                # 扫描进行中：等它，不重复发起
        return
    interval = HEAL_INTERVALS[min(_disc_step, len(HEAL_INTERVALS) - 1)]
    if now - _disc_last_try < interval:
        return
    _disc_last_try = now

    def work():
        global _disc_running, _disc_result
        try:
            _disc_result = (_discover_board_url(),)
        finally:
            _disc_running = False

    _disc_running = True
    threading.Thread(target=work, daemon=True, name="board-discover").start()


def board_state():
    with opener().open(board_url() + "/state", timeout=4) as r:
        _stamp_board_ok()
        return json.loads(r.read())


def board_clear(tool=None, id_prefix=None):
    url = board_url() + "/sessions/clear?"
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
    url = f"{board_url()}/events?event_type={event_type}&src={src}"
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
    url = f"{board_url()}/events?event_type={event_type}&src={src}"
    sid = ""      # #079: 先赋默认——json.loads 抛异常时 except 跳出,
    tool = ""     # 下面 `if not sid` 才不会 UnboundLocalError(今晚坏 body 实测踩中)
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
ZCODE_DB = os.path.join(os.path.expanduser("~"), ".zcode", "cli", "db", "db.sqlite")
_tok_state = {}   # path -> [offset, last_tok, pending_bytes, recreate_at]
_claude_recreate_at = 0.0   # #054: claude 卡重建限流
# #077: 每工具当日消耗（统计页"today"数字）。
# _day_sum[path] = [day, 当日累计, 已计入的字节偏移]——单一代码路径(_crawl_today)
# 增量扫描，按字节偏移防重复计数；只认"时间戳>=本地零点"的 usage 行，
# 所以全天回溯（含看门狗启动前的用量），跨日自动清零。
_day_sum = {}
_last_stats_push = 0.0


def _crawl_today():
    """#077: 扫描今天动过的 Claude 转录，累计当日消耗(input+output+cache_creation)。
    每次只读上次偏移之后的新字节；断电/重启后从文件头回溯全天——
    宁可多扫不可漏算。"""
    import glob as _glob
    from datetime import datetime
    midnight = time.mktime(time.strptime(time.strftime("%Y-%m-%d"), "%Y-%m-%d"))
    today = time.strftime("%Y-%m-%d")
    for path in _glob.glob(CLAUDE_PROJ_GLOB):
        try:
            if os.path.getmtime(path) < midnight:
                continue
            size = os.path.getsize(path)
        except OSError:
            continue
        ds = _day_sum.get(path)
        if ds is not None and ds[0] == today and ds[2] >= size:
            continue
        start_off = ds[2] if (ds is not None and ds[2] > 0) else 0
        total = 0
        try:
            with open(path, "rb") as f:
                f.seek(start_off)
                for raw in f:
                    if b"usage" not in raw:
                        continue
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
                    if not isinstance(u, dict):
                        continue
                    burn = (int(u.get("input_tokens") or 0)
                            + int(u.get("output_tokens") or 0)
                            + int(u.get("cache_creation_input_tokens") or 0))
                    if burn <= 0:
                        continue
                    ts = obj.get("timestamp")
                    if not isinstance(ts, str):
                        continue    # 无时间戳无法归日: 宁少勿错
                    try:
                        if datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp() >= midnight:
                            total += burn
                    except Exception:
                        continue
        except OSError:
            continue
        if ds is None or ds[0] != today:
            ds = [today, 0, 0]
            _day_sum[path] = ds
        ds[1] += total
        ds[2] = size


def push_tool_stats():
    """#077: 每 30s 推每工具当日消耗到板子 POST /stats/tok（绝对值，幂等）。"""
    global _last_stats_push
    now = time.time()
    if now - _last_stats_push < 30:
        return
    _last_stats_push = now
    _crawl_today()
    today = time.strftime("%Y-%m-%d")
    totals = {}
    for day, s, _off in _day_sum.values():
        if day == today:
            totals["claude"] = totals.get("claude", 0) + s
    if _zcode_today >= 0:
        totals["zcode"] = _zcode_today
    # 状态表清理：转录已删除的旧条目（防长跑无限增长）
    if len(_day_sum) > 64:
        for p in list(_day_sum):
            if not os.path.exists(p):
                del _day_sum[p]
    for tool, v in totals.items():
        try:
            url = f"{board_url()}/stats/tok?tool={tool}&today={v}"
            req = urllib.request.Request(url, data=b"{}", method="POST")
            opener().open(req, timeout=2).read()
        except Exception:
            pass    # 失败不重试特殊处理：30s 后下一轮绝对值重推自然覆盖


# ---------- ZCode 用量（#079）----------
# ZCode 日志里的 usage 全被 [Redacted]，真实数据在 db.sqlite 的 model_usage 表
# （每行一次模型请求：session_id/started_at/input/output/cache_*）。
_zcode_today = -1          # 当日 input+output+cache_creation 合计（-1=读库失败）
_zcode_tok_pushed = {}     # sid -> 已推送的当前上下文值（负值=板上无卡，值变了再试）


def zcode_usage_watch():
    """职责六(#079)：ZCode 用量上屏。
      now  = 每会话最新一条请求的上下文(input+cache_read+cache_creation)
             -> POST /sessions/tok 点亮卡片 token
      today = 当日 Σ(input+output+cache_creation) -> push_tool_stats 推统计页
    只读打开(WAL 允许并发读)；库被锁/不存在时静默跳过，下轮再试。"""
    global _zcode_today
    import sqlite3
    try:
        con = sqlite3.connect(f"file:{ZCODE_DB}?mode=ro", uri=True, timeout=2)
    except Exception:
        return
    try:
        cur = con.cursor()
        midnight_ms = time.mktime(
            time.strptime(time.strftime("%Y-%m-%d"), "%Y-%m-%d")) * 1000
        row = cur.execute(
            "SELECT SUM(input_tokens+output_tokens+cache_creation_input_tokens) "
            "FROM model_usage WHERE started_at >= ?", (midnight_ms,)).fetchone()
        _zcode_today = row[0] or 0

        latest = {}
        for sid, it, cr, cc in cur.execute(
                "SELECT session_id, input_tokens, cache_read_input_tokens, "
                "cache_creation_input_tokens FROM model_usage "
                "ORDER BY started_at"):
            if sid:
                latest[sid] = it + cr + cc
        for sid, ctx in latest.items():
            prev = _zcode_tok_pushed.get(sid)
            if prev == ctx:
                continue
            try:
                url = f"{board_url()}/sessions/tok?sid={sid}&tok={ctx}"
                req = urllib.request.Request(url, data=b"{}",
                                             headers={"Content-Type": "application/json"})
                with opener().open(req, timeout=2) as r:
                    resp = json.loads(r.read())
                _zcode_tok_pushed[sid] = ctx if resp.get("updated") else -ctx
            except Exception:
                pass    # 板子不可达: 下轮(10s)重试
    except Exception as e:
        log(f"zcode用量读取异常: {type(e).__name__}: {e}")
    finally:
        con.close()


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
                # 当日消耗的统计不在此处做——单一代码路径在 _crawl_today(#077)，
                # 按字节偏移防重复；此处只管"当前上下文大小"的实时推送
        if tok > 0:
            st[4] = tok
        if st[4] > 0 and st[4] != st[1]:     # known != pushed -> 推送(失败保持-1下轮重试)
            tok = st[4]
            try:
                url = f"{board_url()}/sessions/tok?sid={sid}&tok={tok}"
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
            if not body.strip():
                os.remove(path)     # #065B: 空 body 事件(幽灵源)直接丢弃
                log(f"空 body 事件丢弃: {name}")
                continue
            try:                    # #079: 非 JSON body(实测出现过 base64 乱码)转发
                json.loads(body)    # 到板上只会落进"?"幽灵桶——源头丢弃
            except Exception:
                os.remove(path)
                log(f"非JSON事件丢弃: {name} body[:40]={body[:40]!r}")
                continue
            board_event_raw(ev, src, body, tok)
            _stamp_board_ok()      # #087: 任何一次成功通信都证明板子活着
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
    trae_alive = any(n == "trae cn.exe" for n in names)   # #080: Trae CN

    st = board_state()
    cards = st.get("table", [])
    claude_cards = [c for c in cards if c.get("tool") == "claude"]
    zcode_cards = [c for c in cards if c.get("tool") == "zcode"]
    trae_cards = [c for c in cards if c.get("tool") == "trae"]

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
            # #061: 窗口 15min——重建只服务"刚被误清的活会话"(几分钟前还有事件);
            # 长期不活跃的旧会话不该被重建(转录活跃≠会话活着,#060)
            files = []
            for f in _g.glob(CLAUDE_PROJ_GLOB):
                try:
                    if now2 - os.path.getmtime(f) < 900:      # 15分钟(#061)
                        files.append(f)
                except OSError:
                    pass   # #065修复5: glob与getmtime之间文件被删,跳过该文件
            for f in files:
                sid = os.path.basename(f)[:-6]
                enqueue_event("session-start", "claude",
                              json.dumps({"session_id": sid}).encode())
                # #062: 按转录尾部推断真实状态——不要再无脑发 stop(置DONE)。
                # 最后 assistant 消息含 tool_use = 等审批/执行中 → ERROR(红闪/黄闪);
                # 纯 text = 回合完成 → DONE(绿)。卡状态与电脑实况一致。
                infer = "stop"
                try:
                    with open(f, "rb") as fh:
                        fh.seek(max(0, os.path.getsize(f) - 65536))
                        tail_lines = fh.read().decode("utf-8", "replace").split("\n")
                    for raw in reversed(tail_lines):
                        raw = raw.strip()
                        if not raw.startswith("{"):   # #065修复1: raw 是 str, b"{" 会 TypeError 被兜底吞掉
                            continue
                        obj = json.loads(raw)
                        if obj.get("type") != "assistant":
                            continue
                        msg = obj.get("message", {})
                        c = msg.get("content")
                        kinds = [i.get("type") for i in c
                                 if isinstance(i, dict)] if isinstance(c, list) else []
                        infer = ("permission-request" if "tool_use" in kinds
                                 else "stop")
                        break
                except Exception:
                    pass
                enqueue_event(infer, "claude",
                              json.dumps({"session_id": sid}).encode())
                log(f"claude 无卡 -> 重建 {sid[:12]} 恢复状态={infer} (#062)")

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

    if trae_cards and not trae_alive:
        r = board_clear(tool="trae")
        log(f"Trae 未运行但板上 {len(trae_cards)} 卡 -> 全清 {r}")


def main() -> int:
    # 单实例锁(#087)：双开实例会抢队列(FileNotFoundError 互踩) + 双份补发。
    # 实测事故：一个普通实例 + 一个提权实例(用户手动起)并存。
    try:
        lock_dir = os.path.join(os.path.expanduser("~"), ".ai_status")
        lock = os.path.join(lock_dir, "watchdog.pid")
        os.makedirs(lock_dir, exist_ok=True)
        try:
            old = int(open(lock).read().strip() or 0)
        except Exception:
            old = 0
        if old and old != os.getpid():
            out = subprocess.run(
                ["tasklist", "/FI", f"PID eq {old}", "/FO", "CSV", "/NH"],
                capture_output=True, timeout=10,
                creationflags=0x08000000).stdout.decode("gbk", "replace")
            if "pythonw.exe" in out:
                # 名字命中后再看命令行：PID 可能被无关 pythonw 复用。
                # 提权进程读不到命令行（空串）——此时退回按名字判定
                cl = ""
                try:
                    r = subprocess.run(
                        ["powershell", "-NoProfile", "-Command",
                         f"(Get-CimInstance Win32_Process "
                         f"-Filter \"ProcessId={old}\").CommandLine"],
                        capture_output=True, timeout=20,
                        creationflags=0x08000000)
                    cl = r.stdout.decode("gbk", "replace")
                except Exception:
                    pass
                if not cl.strip() or "esp_light_watchdog.py" in cl:
                    log(f"已有实例在跑(pid={old})，本实例退出（防双开抢队列）")
                    return 0
                log(f"pid {old} 是别的 pythonw（PID 复用），接管锁")
        with open(lock, "w") as f:
            f.write(str(os.getpid()))
    except Exception:
        pass   # 锁机制本身不许把看门狗锁死

    log("看门狗启动(v5: 队列转发 + 进程巡检 + 批准加速 + 实时token + 地址自愈#087)")
    tailer = ZCodeLogTailer(ZCODE_LOG_GLOB)
    tick = 0
    while True:
        tick += 1
        # 职责四：实时 token 监视（每 1 秒，直接跟 Claude 转录文件 #043）
        try:
            claude_token_watch()
        except Exception as e:
            log(f"token监视异常: {type(e).__name__}: {e}")

        # 职责五：每工具当日消耗推送（每 30s，#077 统计页）
        try:
            push_tool_stats()
        except Exception as e:
            log(f"统计推送异常: {type(e).__name__}: {e}")

        # 职责六：ZCode 用量上屏（每 10s 读 model_usage，#079）
        if tick % 10 == 0:
            try:
                zcode_usage_watch()
            except Exception as e:
                log(f"zcode用量异常: {type(e).__name__}: {e}")

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

        # 职责七（#087）：板子失联自愈——地址变了自动找回，手机配网后 PC 零操作
        try:
            auto_heal_tick()
        except Exception as e:
            log(f"自愈异常: {type(e).__name__}: {e}")

        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
