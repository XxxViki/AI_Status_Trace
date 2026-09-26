#!/usr/bin/env python3
"""跑 stress_matrix 同时抓串口，找出 D 部分失败的真因（#055 排查）"""
import subprocess
import sys
import time

PY = r"C:\Users\11264\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe"
CAP = r"C:\Users\11264\AppData\Local\Temp\serial_cap.txt"

cap_code = (
    "import serial, time\n"
    "s = serial.Serial('COM9', 115200, timeout=0.5)\n"
    "end = time.time() + 115\n"
    "buf = b''\n"
    "while time.time() < end:\n"
    "    buf += s.read(4096)\n"
    "s.close()\n"
    f"open(r'{CAP}', 'wb').write(buf)\n"
)

p = subprocess.Popen([PY, "-c", cap_code])
time.sleep(2)

r = subprocess.run([sys.executable, "tools/stress_matrix.py", "--fast"],
                   capture_output=True, text=True, timeout=150)
# 只打印 D 段
lines = r.stdout.splitlines()
d_start = next(i for i, l in enumerate(lines) if "[D]" in l)
d_end = next(i for i, l in enumerate(lines) if "[E]" in l)
print("\n".join(lines[d_start:d_end]))

p.wait(timeout=30)

cap = open(CAP, "rb").read().decode("utf-8", errors="replace")
scales = [l for l in cap.splitlines() if "缩放" in l]
print("\n=== 串口时间缩放记录（按序） ===")
for l in scales[-10:]:
    print(l[:100])
