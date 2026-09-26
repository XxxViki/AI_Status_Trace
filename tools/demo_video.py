#!/usr/bin/env python3
"""
15 秒演示脚本：卡片全状态展示（供录视频）

用法: python tools/demo_video.py
倒计时 3 秒后开始，全程 15 秒，结束自动清屏。

时间线:
  0.0s  1 卡 WORKING(黄呼吸) — 铺满 312px
  1.8s  +APPROVE(黄闪) → 2卡各154px, 抢到第1位
  3.6s  +DONE(绿) → 3卡各101px
  5.4s  +第4张 → 折叠进头部 mini-logo
  7.2s  审批升级 → 红闪跳第1位
  9.0s  各卡陆续完成/结束 → 卡片逐张消失
  12.0s 只剩1张 → 自动铺满
  14.0s 清屏完成
"""
import json
import sys
import time
import urllib.request

BOARD = "http://192.168.1.20"
DURATION = 15.0


def op():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def post(path, body=None, timeout=3, retries=3):
    url = BOARD + path
    for _ in range(retries):
        try:
            r = urllib.request.Request(url, data=json.dumps(body or {}).encode(),
                                       headers={"Content-Type": "application/json"})
            with op().open(r, timeout=timeout) as resp:
                return resp.read().decode()
        except Exception:
            time.sleep(0.3)
    return None


def ev(etype, sid, src="zcode", cwd="D:/work/demo"):
    return post(f"/events?event_type={etype}&src={src}",
                {"session_id": sid, "cwd": cwd})


def clear_all():
    post("/sessions/clear", {})


def at(t_target, label):
    """等到指定时间点（从 t0 起算）"""
    now = time.time() - t0
    if now < t_target:
        time.sleep(t_target - now)
    print(f"  [{t_target:4.1f}s] {label}")


t0 = 0.0

def main():
    global t0
    print("=" * 44)
    print("  15 秒全状态演示 — 准备录制！")
    print("=" * 44)
    for i in (3, 2, 1):
        print(f"  {i}...")
        time.sleep(0.8)
    print("  ▶ 开始！")
    print()

    t0 = time.time()

    # 清屏(保留底噪最少)
    clear_all()
    time.sleep(0.3)
    clear_all()

    # ===== 时间线 =====

    # 0.0s: 1 卡 WORKING — 铺满
    at(0.0, "① 1卡 WORKING(黄呼吸) 铺满312px")
    ev("pre-tool-use", "demo-1-w1", src="claude", cwd="D:/work/1-alpha")

    # 1.8s: +APPROVE → 2卡
    at(1.8, "② +APPROVE(黄闪) → 2卡154px, APPROVE抢第1位")
    # ts=22: 升级点 = 本事件 + 120000/22 ≈ +5.5s = 7.3s → 正好落在⑤
    ev("permission-request&ts=22", "demo-2-a1", src="zcode", cwd="D:/work/2-beta")

    # 3.6s: +DONE → 3卡
    at(3.6, "③ +DONE(绿) → 3卡101px")
    ev("stop", "demo-3-d1", src="trae", cwd="D:/work/3-gamma")

    # 5.4s: +第4张 → 折叠
    at(5.4, "④ +第4张 → 折叠进头部mini-logo(半暗)")
    ev("pre-tool-use", "demo-4-w2", src="zcode", cwd="D:/work/4-delta")

    # 7.2s: 审批升级 → 红闪跳第1
    at(7.2, "⑤ 审批升级 → 红闪跳第1位(ts=22自然触发,本步无事件)")
    # #056/#057: 此步不发事件——旧版用 pre-tool-use 会把卡改回 WORKING,红闪出不来。
    # 改为 ②的 permission 自带 ts=22(升级点=+5.5s=7.3s), 红闪自然出现在本步。

    # 9.0s: 各卡完成 → 绿闪
    at(9.0, "⑥ 各卡完成 → 绿闪/排序刷新")
    ev("stop", "demo-1-w1", src="claude")       # alpha 完成(绿闪)
    ev("stop", "demo-4-w2", src="zcode")       # delta 完成

    # 10.8s: 逐张消失
    at(10.8, "⑦ 逐张结束 → 卡片消失")
    ev("session-end", "demo-3-d1")              # gamma 消失
    ev("session-end", "demo-2-a1")              # beta 消失

    # 12.6s: 只剩 alpha → 铺满
    at(12.6, "⑧ 只剩1张 → 自动铺满")

    # 14.0s: 清屏
    at(14.0, "⑨ 清屏")
    # #058: 清理清单从"手抄会话名"改为"逐个已知会话 + 断言板上无 demo-* 残留"
    ALL_DEMO = ["demo-1-w1", "demo-2-a1", "demo-3-d1", "demo-4-w2"]
    for sid in ALL_DEMO:
        ev("session-end", sid)
    # #059: ts-rst 用 session-end(pre-tool-use 会闪现一张 WORKING 卡)
    post("/events?event_type=session-end&ts=1", {"session_id": "ts-rst"})

    elapsed = time.time() - t0
    print(f"\n  ✅ 演示完成 ({elapsed:.1f}s)")

    # 断言: 板上不允许残留任何 demo-* 卡（#058 用户要求：测试完顺手清）
    time.sleep(1.5)
    leftover = []
    for _ in range(4):
        st = post("/state")
        if st:
            leftover = [c["id"] for c in json.loads(st)["table"]
                        if c["id"].startswith("demo-")]
            if not leftover:
                break
            for sid in leftover:
                ev("session-end", sid)
            time.sleep(1)
    if leftover:
        print("  残留检查: ✗ 仍有残留 " + str(leftover))
    else:
        print("  残留检查: 干净 ✓")
    print("  ts 已恢复")


if __name__ == "__main__":
    main()
