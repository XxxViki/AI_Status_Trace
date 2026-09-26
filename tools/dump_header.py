#!/usr/bin/env python3
"""头部 mini-logo 区域逐像素 ASCII 渲染（排查 logo 显示问题 #056）"""
import time
import sys
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def row(y, step=1):
    for _ in range(6):
        try:
            with opener.open(
                    "http://192.168.1.20/dbg/row?y=%d&step=%d" % (y, step),
                    timeout=5) as r:
                s = r.read().decode().strip()
            if len(s) >= 1280:                    # 320点 x 4hex 全量
                px = []
                for i in range(0, len(s) - 3, 4):
                    chunk = s[i:i + 4].upper()
                    if all(ch in "0123456789ABCDEF" for ch in chunk):
                        px.append(chunk)
                    else:
                        px.append(hdr_bg)         # 坏块按背景处理, 避免 '?' 假象
                while len(px) < 320:
                    px.append(hdr_bg)
                return px[:320]
        except Exception:
            pass
        time.sleep(1)
    raise RuntimeError("row %d 读取失败" % y)


def main():
    hdr_bg = "2125"
    frame = [row(y) for y in range(2, 26)]
    print("头部格子逐像素 (x=110..319, 每列2px):")
    for y_i, pxrow in enumerate(frame):
        line = ""
        for x in range(110, 320, 2):
            c = pxrow[x]
            if c == hdr_bg:
                line += " "
                continue
            v = int(c, 16)
            r5 = (v >> 11) & 0x1F
            g6 = (v >> 5) & 0x3F
            b5 = v & 0x1F
            lum = r5 * 2 + g6 * 3 + b5
            line += "#" if lum > 60 else "+"
        print("%2d %s" % (2 + y_i, line))
    print()
    print("图例: #=亮色(品牌色/白) +=暗色 空格=表头底色")


if __name__ == "__main__":
    main()
