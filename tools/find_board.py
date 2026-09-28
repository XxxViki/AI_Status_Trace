#!/usr/bin/env python3
"""板子 IP 发现（#085）—— 换网段后不用再猜地址

背景：板子地址曾写死在 4 个脚本里（家里 DHCP 保留 192.168.1.20）。换到公司
10.0.x.x 网段后，板子在网里活着但没人知道它的新 IP，整套系统看起来"全断了"。

做法（三步，都不需要板子配合改固件）：
  1. 对本机所在 /24 做 ping 扫描 —— ICMP 回不回不重要，目的是让 ARP 表填满
  2. 在 ARP 表里按 MAC 找板子 —— ESP32 的 MAC 烧录即固定，比 IP 稳得多
  3. 用 GET /health 确认身份（MAC 命中 + HTTP 活着 = 就是它）

用法:
  python tools/find_board.py                  # 扫本机 /24，只打印结果
  python tools/find_board.py --write          # 找到后写入 ~/.ai_status/board_url
  python tools/find_board.py --subnets 10.0.2,10.0.7
  python tools/find_board.py --mac 9c:cc:01   # 只按前缀匹配（多板子时先看列表）

注意：公司网段若是 /16 大网、板子又不在本机 /24 里，请显式 --subnets 指定
或先向网管要 IP —— 全 /16 扫描对办公网不友好，本工具不做。
"""
import argparse
import concurrent.futures as futures
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "hook"))
from board_addr import BOARD_URL_FILE, board_url   # noqa: E402

# 板子 MAC（esptool 读出，烧录即固定）：换板子/换芯片时改这里
BOARD_MAC = "9c:cc:01:d9:30:1c"
ARP_ROW = re.compile(r"^\s*(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F]{2}(?:-[0-9a-fA-F]{2}){5})")


def local_subnets() -> list:
    """本机 IPv4 所在的 /24（socket 取地址，避开中英文本地化解析）"""
    subs = []
    try:
        import socket
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if ip.startswith(("127.", "169.254.")):
                continue
            subs.append(".".join(ip.split(".")[:3]))
    except OSError:
        pass
    return sorted(set(subs))


def arp_table() -> dict:
    """{ip: mac} —— Windows arp -a 输出（大小写/分隔符都归一化）"""
    out = subprocess.run(["arp", "-a"], capture_output=True,
                         creationflags=0x08000000)
    text = out.stdout.decode("gbk", "replace")
    table = {}
    for line in text.splitlines():
        m = ARP_ROW.match(line)
        if m:
            table[m.group(1)] = m.group(2).replace("-", ":").lower()
    return table


def ping(ip: str) -> None:
    subprocess.run(["ping", "-n", "1", "-w", "600", ip],
                   capture_output=True, creationflags=0x08000000)


def sweep(subnets: list, workers: int = 64) -> None:
    targets = [f"{s}.{i}" for s in subnets for i in range(1, 255)]
    print(f"扫描 {len(subnets)} 个网段 / 共 {len(targets)} 个地址 ...")
    with futures.ThreadPoolExecutor(max_workers=workers) as ex:
        list(ex.map(ping, targets))


def health_ok(ip: str) -> bool:
    import urllib.request
    try:
        op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with op.open(f"http://{ip}/health", timeout=3) as r:
            return r.status == 200
    except Exception:
        return False


def write_board_url(url: str) -> None:
    """写入配置文件：保留原文件里的注释与其它行，只替换/追加生效行。
    先写临时文件再 os.replace——看门狗每秒都在读，截断-重写会让读者
    在一瞬间缓存到半行内容（评审修复#087）"""
    Path(BOARD_URL_FILE).parent.mkdir(parents=True, exist_ok=True)
    lines = []
    if os.path.exists(BOARD_URL_FILE):
        with open(BOARD_URL_FILE, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    out, replaced = [], False
    for line in lines:
        s = line.strip()
        if s and not s.startswith("#"):
            if not replaced:
                out.append(url)      # 第一条生效行被替换，其余（旧网络）注释掉
                replaced = True
            else:
                out.append("# " + line)
        else:
            out.append(line)
    if not replaced:
        out.append(url)
    tmp = BOARD_URL_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(out) + "\n")
    os.replace(tmp, BOARD_URL_FILE)


def main() -> int:
    ap = argparse.ArgumentParser(description="按 MAC 在局域网里找板子")
    ap.add_argument("--subnets", help="逗号分隔的 /24 前缀，如 10.0.2,10.0.7")
    ap.add_argument("--mac", default=BOARD_MAC, help=f"板子 MAC（默认 {BOARD_MAC}）")
    ap.add_argument("--write", action="store_true",
                    help=f"找到后写入 {BOARD_URL_FILE}")
    args = ap.parse_args()

    want = args.mac.lower().replace("-", ":")
    subnets = ([s.strip() for s in args.subnets.split(",") if s.strip()]
               if args.subnets else local_subnets())
    if not subnets:
        print("!! 拿不到本机网段，请用 --subnets 指定（如 --subnets 10.0.2）")
        return 2
    print(f"当前配置地址: {board_url()}")

    # 前缀匹配（文档约定：--mac 9c:cc:01 可用于多板子时先列候选）
    def hit(mac: str) -> bool:
        return mac.startswith(want)

    for attempt in (1, 2):          # 扫两遍：首扫可能有 ARP 条目过期
        sweep(subnets)
        found = [ip for ip, mac in arp_table().items() if hit(mac)]
        if found:
            break
        if attempt == 1:
            print("第一遍未命中，再扫一遍 ...")
    if not found:
        print(f"!! 未找到 MAC={want} 的设备。可能原因：\n"
              f"   - 板子不在本机这些网段: {', '.join(subnets)}（公司 /16 大网请 --subnets）\n"
              "   - 板子没连上 WiFi（长按 BOOT 3 秒进配网热点 AI-Status-Setup 重新配网）")
        return 1

    for ip in found:
        ok = health_ok(ip)
        print(f"MAC 命中: {ip}  /health -> {'ok' if ok else '无响应'}")
        if ok:
            url = f"http://{ip}"
            if args.write:
                write_board_url(url)
                print(f"已写入 {BOARD_URL_FILE}（看门狗 1~3 秒内自动生效）")
            else:
                print(f"（加 --write 可写入 {BOARD_URL_FILE}）")
            return 0
    print("!! MAC 对上了但 HTTP 不通，板子可能在重启或换了网段")
    return 1


if __name__ == "__main__":
    sys.exit(main())
