#!/usr/bin/env python3
"""板子地址统一解析（#085）

背景：所有脚本曾各自写死 http://192.168.1.20（家里的 DHCP 保留地址），
换成公司 10.0.x.x 网段后全部失联。收敛到单一来源，换网络只改一处。

解析优先级：
  1. 环境变量 AI_STATUS_BOARD（临时覆盖，如 CI/调试）
  2. ~/.ai_status/board_url 的第一个非注释行（常驻配置，推荐）
  3. 兜底默认（家里的地址）

文件按 mtime 检测变化：看门狗常驻进程里每次调用都走这里，
改完文件下一个轮询周期（1~3s）自动生效，无需重启看门狗。
烧录带 mDNS 的固件后，文件里可以直接写 http://aistatus.local。
"""
import os

BOARD_URL_FILE = os.path.join(os.path.expanduser("~"), ".ai_status", "board_url")
BOARD_DEFAULT = "http://192.168.1.20"

_cache = [None, BOARD_DEFAULT]   # [board_url 文件的 mtime, 当前地址]


def board_url() -> str:
    env = os.environ.get("AI_STATUS_BOARD")
    if env:
        return env.rstrip("/")
    try:
        mtime = os.path.getmtime(BOARD_URL_FILE)
    except OSError:
        return BOARD_DEFAULT          # 文件不存在：用默认（家里）
    if _cache[0] != mtime:
        try:
            with open(BOARD_URL_FILE, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        _cache[1] = line.rstrip("/")
                        break
        except OSError:
            pass
        _cache[0] = mtime
    return _cache[1]
