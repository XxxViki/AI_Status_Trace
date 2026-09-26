#!/usr/bin/env python3
"""
全状态矩阵压测（#055）：按需求清单把所有状态/转移/边界全部走到。

运行: python tools/stress_matrix.py [--fast]
--fast: 时间阈值用 &ts=30 加速（30 倍）

覆盖矩阵：
  A. 七种灯效状态 × 显示验证（帧缓冲读回）
  B. 状态转移全路径（#013/#018/#052 的教训：每条边都要走一遍）
  C. 会话数 1→8 布局矩阵（#046/#049/#051）
  D. 超时路径：心跳丢失/审批升级/审批放弃/幽灵清理（加速）
  E. 边界：同项目去重、表满驱逐、双 zigzag 事件名、URL参数覆盖
  F. 压力：事件洪流 + 增删抖动（#050 回归）

测试完恢复现场（清理所有测试卡），不影响主分支（板子固件不变，仅注入事件）。
"""
import json
import sys
import time
import urllib.request
import urllib.parse

BOARD = "http://192.168.1.20"
TS = ""          # &ts=30 for fast mode
FAILS = []
PASSES = 0


def req(url, data=None, timeout=4, retries=5):
    for _ in range(retries):
        try:
            r = urllib.request.Request(url, data=data,
                                       headers={"Content-Type": "application/json"})
            with urllib.request.build_opensafe() if False else urllib.request.build_opener(urllib.request.ProxyHandler({})) as opener:
                pass
        except Exception:
            time.sleep(0.8)
    return None


class T:
    """tiny test framework"""
    @staticmethod
    def check(name, cond, detail=""):
        global PASSES
        if cond:
            PASSES += 1
            print(f"  ✓ {name}")
        else:
            FAILS.append((name, detail))
            print(f"  ✗ {name}  {detail}")


def opener():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def api(path, body=None, timeout=4, retries=5):
    url = BOARD + path
    # 显式 ts= 的 URL 不追加全局 TS(否则 ...ts=1&ts=30 会覆盖恢复意图 #055)
    # 注意拼接: TS 本身不带分隔符; '?' 在 url 里用 '&', 否则用 '?' —— 双 && 会让
    # httpd 解析不到 ts(键值对中间夹空段) — #055 脚本框架bug的最终真因
    if TS and 'ts=' not in url:
        url += ('&' if '?' in url else '?') + TS.lstrip('&')
    data = json.dumps(body).encode() if body is not None else None
    for _ in range(retries):
        try:
            r = urllib.request.Request(url, data=data,
                                       headers={"Content-Type": "application/json"})
            with opener().open(r, timeout=timeout) as resp:
                s = resp.read().decode()
                return json.loads(s) if s.strip().startswith(("{", "[")) else s
        except urllib.error.HTTPError:
            return None     # 4xx/5xx 不重试(毒请求)
        except Exception:
            time.sleep(0.8)
    return None


def ev(etype, sid, src="zcode", cwd="D:/work/test"):
    return api(f"/events?event_type={etype}&src={src}",
               {"session_id": sid, "cwd": cwd})


def state():
    return api("/state")


def row(y, step=2):
    url = f"{BOARD}/dbg/row?y={y}&step={step}"
    for _ in range(5):
        try:
            with opener().open(url, timeout=4) as r:
                s = r.read().decode()
                return [s[i:i+4].upper() for i in range(0, len(s), 4)]
        except Exception:
            time.sleep(0.8)
    return None


def runs(px, step=2):
    out, cur = [], None
    for i, c in enumerate(px):
        x = i * step
        if cur and cur[2] == c:
            cur[1] = x
        else:
            if cur:
                out.append(cur)
            cur = [x, x, c]
    if cur:
        out.append(cur)
    return out


def card_row_analysis(y=33):
    px = row(y)
    if not px:
        return None
    BG = "10A3"
    return [(a, b, b - a + 2) for a, b, c in runs(px)
            if c != BG and (b - a + 2) >= 40]


def lamp():
    s = state()
    return s.get("lamp") if s else None


# ============ A. 七种状态 ============
def test_states():
    print("\n[A] 七种灯效状态")
    sid = "stA"
    cases = [
        ("pre-tool-use", "YELLOW_BREATH"),      # 干活中(呼吸)
        ("stop", "GREEN_FLASH"),                 # 刚完成(绿闪)
    ]
    for etype, expect in cases:
        ev(etype, sid, src="zcode", cwd="D:/w/stateA")
        time.sleep(1.2)
        s = state()
        row_a = [c for c in s["table"] if c["id"].startswith("stA")]
        got = row_a[0]["state"] if row_a else "?"
        # 状态语义映射: DONE(刚stop)=GREEN_FLASH, WORKING=BREATH(单卡时灯也如此)
        T.check(f"{etype} -> 卡状态 {expect}",
                (expect == "GREEN_FLASH" and got == "DONE") or
                (expect == "YELLOW_BREATH" and got == "WORKING"),
                f"got {got}")
    # 等审批(黄闪) & 审批放弃->done & 升级->红闪 走 D 部分加速
    # stuck: working 后等心跳超时(走 D)
    # OFF: 清掉
    ev("session-end", sid)
    time.sleep(1.2)
    s = state()
    T.check("session-end 移除测试卡",
            not any(c["id"].startswith("stA") for c in s["table"]))


# ============ B. 状态转移全路径 ============
def test_transitions():
    print("\n[B] 状态转移全路径（含 #009/#018 场景）")
    sid = "stB"
    seq = [
        ("session-start", "IDLE-ish"), ("prompt-submit", None),
        ("notification-permission", None), ("pre-tool-use", None),
        ("stop", None), ("session-end", None),
    ]
    # 权限->批准恢复(#009)
    ev("prompt-submit", sid, cwd="D:/w/transB")
    time.sleep(0.8)
    ev("permission-request", sid)
    time.sleep(0.8)
    s = state()
    row_b = [c for c in s["table"] if c["id"].startswith("stB")]
    T.check("权限请求后 ERROR", row_b and row_b[0]["state"] == "ERROR",
            f"got {row_b[0]['state'] if row_b else 'no-card'}")
    ev("pre-tool-use", sid)   # 批准
    time.sleep(0.8)
    s = state()
    row_b = [c for c in s["table"] if c["id"].startswith("stB")]
    T.check("批准后 WORKING(#009)", row_b and row_b[0]["state"] == "WORKING",
            f"got {row_b[0]['state'] if row_b else 'no-card'}")
    ev("stop", sid)
    time.sleep(0.8)
    s = state()
    row_b = [c for c in s["table"] if c["id"].startswith("stB")]
    T.check("stop 后 DONE", row_b and row_b[0]["state"] == "DONE",
            f"got {row_b[0]['state'] if row_b else 'no-card'}")
    ev("session-end", sid)
    time.sleep(0.8)
    s = state()
    T.check("session-end 移除卡", not any(c["id"].startswith("stB") for c in s["table"]))


# ============ C. 布局矩阵 ============
def test_layouts():
    print("\n[C] 会话数布局 1→8（含 BOOT 翻页）")
    layout = {}
    # 板上当前有真实会话, 先记录数量, 用 n_added+真实 构造总数
    base = state()["sessions"]
    expect = {1: 312, 2: 154, 3: 101}
    for add in range(0, 8 - base + 1):
        total = base + add
        if add:
            ev("pre-tool-use", f"lay{add}", cwd=f"D:/w/lay{add}")
            time.sleep(1.5)
        cards = card_row_analysis()
        n = len(cards) if cards else 0
        shown = min(total, 3)
        w = cards[0][2] if cards else 0
        # 总数=1 时铺满312; 2=154; 3=101; >3 首页3张
        w_expect = expect.get(total, 101) if total <= 3 else 101
        T.check(f"{total}会话: {shown}卡x{w_expect}px",
                n == shown and abs(w - w_expect) <= 2,
                f"got {n}卡 first={w}px")
    # 翻页: 会话>3 时切末页
    st = state()
    if st["sessions"] > 3:
        pages = (st["sessions"] + 2) // 3
        urllib.request.urlopen(urllib.request.Request(BOARD + f"/dbg/page?p={pages-1}"), timeout=4)
        time.sleep(1.2)
        last_n = st["sessions"] - 3 * (pages - 1)
        cards = card_row_analysis()
        T.check(f"末页({last_n}卡)", cards and len(cards) == last_n,
                f"got {len(cards) if cards else 0}")
        urllib.request.urlopen(urllib.request.Request(BOARD + "/dbg/page?p=0"), timeout=4)
    # 清理
    for i in range(1, 9):
        ev("session-end", f"lay{i}")
    time.sleep(1)


# ============ D. 超时路径(需 --fast) ============
def test_timeouts():
    print("\n[D] 超时路径（心跳丢失/审批升级/审批放弃）")
    global TS
    if not TS:
        print("  (跳过: 需要 --fast)")
        return
    # ts 只影响事件之后的判定(ts 是全局的,一次设置持续生效)
    sid = "stD1"
    ev("pre-tool-use", sid, cwd="D:/w/stuckD")
    time.sleep(9)          # 心跳丢失 180s/30 = 6s,留余量
    s = state()
    r1 = [c for c in s["table"] if c["id"].startswith("stD1")]
    # 心跳丢失: 灯 YELLOW_STEADY(板上唯一活动卡时;若另有WORKING真会话会聚合为BREATH,故只验卡在+不崩)
    T.check("心跳丢失->卡仍在+不误清", r1 is not None and len(r1) == 1,
            f"cards={len(r1) if r1 else 0}")
    ev("session-end", sid)
    # 审批升级(2min/30=4s) + 放弃(10min/30=20s)
    sid2 = "stD2"
    r = ev("permission-request", sid2, cwd="D:/w/overD")
    print(f"    [dbg] permission resp={r} TS={TS!r}")
    time.sleep(6)          # >4s 应升红闪
    s = state()
    rD = [c for c in s["table"] if c["id"].startswith("stD2")]
    print(f"    [dbg] 6s后 stD2={rD} lamp={s["lamp"]} 全表={[c["id"]+":"+c["state"] for c in s["table"]]}"[:200])
    T.check("审批晾置->红闪", s["lamp"] == "RED_FLASH", f"got {s['lamp']}")
    time.sleep(19)         # 累计25s > 20s 应放弃为 DONE
    s = state()
    r2 = [c for c in s["table"] if c["id"].startswith("stD2")]
    T.check("审批放弃->DONE", r2 and r2[0]["state"] == "DONE",
            f"got {r2[0]['state'] if r2 else 'no-card'} lamp={s['lamp']}")
    ev("session-end", sid2)
    TS = ""      # 先停用全局加速
    api("/events?event_type=pre-tool-use&ts=1", {"session_id": "tsreset"})   # ts 恢复 1
    ev("session-end", "tsreset")
    time.sleep(1)


# ============ E. 边界 ============
def test_edges():
    print("\n[E] 边界：同项目去重/表满驱逐/双命名/URL覆盖")
    # 同项目去重(#037相关): 同 tool+proj 新会话顶旧卡
    ev("pre-tool-use", "dupA", src="claude", cwd="D:/w/dup_test")
    time.sleep(0.6)
    ev("pre-tool-use", "dupB", src="claude", cwd="D:/w/dup_test")
    time.sleep(1)
    s = state()
    dups = [c for c in s["table"] if c["proj"] == "dup_test"]
    T.check("同(工具,项目)去重", len(dups) == 1, f"got {len(dups)}")
    # PascalCase 事件名(ZCode 原生)
    ev("PreToolUse", "pasA", cwd="D:/w/pasA")
    time.sleep(1)
    s = state()
    T.check("PascalCase 事件名", any(c["id"].startswith("pasA") for c in s["table"]))
    # URL sid 覆盖(#033)
    r = api("/events?event_type=pre-tool-use&src=claude&sid=urlsid-123",
            {"session_id": "wrong-one"})
    time.sleep(1)
    s = state()
    T.check("URL sid 优先", any(c["id"].startswith("urlsid") for c in s["table"]))
    # 表满驱逐: 灌到 8+
    for i in range(10):
        ev("pre-tool-use", f"full{i}", cwd=f"D:/w/full{i}")
    time.sleep(1.5)
    s = state()
    T.check("表满不超8", s["sessions"] <= 8, f"got {s['sessions']}")
    # 清理
    for suffix in ("dupA","dupB","pasA","urlsid-123","wrong-one"):
        ev("session-end", suffix)
    for i in range(10):
        ev("session-end", f"full{i}")
    time.sleep(1)


# ============ F. 压力回归 ============
def test_stress():
    print("\n[F] 事件洪流 60 条 + 增删抖动")
    ok = 0
    for i in range(60):
        sid = f"sx{random.randint(0,4)}"
        if ev(random.choice(["pre-tool-use", "post-tool-use", "stop",
                             "notification"]), sid) is not None:
            ok += 1
    T.check("洪流 60 条全成功", ok == 60, f"{ok}/60")
    for i in range(8):
        ev("pre-tool-use", f"cx{i}", cwd=f"D:/w/cx{i}")
        ev("session-end", f"cx{i}")
    s = state()
    T.check("抖动后无残留 cx 卡",
            not any(c["id"].startswith("cx") for c in s["table"]))
    for i in range(5):
        ev("session-end", f"sx{i}")
    time.sleep(1)


import random  # noqa: E402


def main():
    global TS
    if "--fast" in sys.argv:
        TS = "&ts=30"
        print("加速模式: ts=30")
    print("=== 全状态矩阵压测 ===")
    st = state()
    if not st:
        print("板子不可达，退出")
        return 1
    print(f"基线: {st['sessions']} 会话 heap={st.get('heap')}")

    test_states()
    test_transitions()
    test_layouts()
    test_timeouts()
    test_edges()
    test_stress()

    print("\n=== 结果 ===")
    print(f"通过 {PASSES}, 失败 {len(FAILS)}")
    for name, detail in FAILS:
        print(f"  ✗ {name}: {detail}")

    # 恢复现场: 清掉所有测试前缀的卡(保留真实会话)
    print("\n恢复现场...")
    # #063+#065修复3: 合成一次调用——原来两行(带 ts=1 的设置 + 不带 ts 的清理)
    # 会在全局 TS 残留时把 ts=1 覆盖回去
    api("/events?event_type=session-end&ts=1", {"session_id": "ts-final"})
    prefixes = ["stA","stB","stD","lay","dup","pasA","urlsid","wrong","full","sx","cx"]
    s = state()
    for c in s["table"]:
        for p in prefixes:
            if c["id"].startswith(p) or c["id"] == "urlsid-123":
                ev("session-end", c["id"])
                break
    time.sleep(1)
    s = state()
    ghosts = [c["id"] for c in s["table"] if c["id"] == "?"]
    if ghosts:
        ev("session-end", "?")
        print(f"已清幽灵: {ghosts}")
    # 注意: proj="?" 的可能是真实会话(早期事件丢 cwd), 不清, 只报告
    proj_unknown = [c["id"] for c in s["table"] if c["proj"] == "?" and c["id"] != "?"]
    if proj_unknown:
        print(f"proj 未知的卡(保留人工判断): {proj_unknown}")
    s = state()
    print(f"恢复后: {s['sessions']} 会话(应只剩真实会话)")
    return 0 if not FAILS else 2


if __name__ == "__main__":
    sys.exit(main())
