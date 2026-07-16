#!/usr/bin/env python3
# Drive the on-glass perf probe (src_cyd/perf_probe.cpp, env esp32dev_cyd35_perf) and print its
# [perf] report. Usage: perf-device.py PORT [CMD]  (CMD defaults "a").
#
# PORT is required and has no default: with multiple devkits attached the ttyUSBn numbering is
# random per plug order, so identify the board by its ESP32 MAC (see CLAUDE.md) and pass the
# confirmed device. `make perf-device` resolves it via tools/resolve-port.sh.
#
# The firmware talks 115200 regardless of monitor_speed (Serial.begin(115200) in main.cpp), and
# dtr/rts are held off so opening the port does not reset the board mid-measurement.
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed: pip install pyserial")

if len(sys.argv) < 2:
    sys.exit("usage: perf-device.py PORT [CMD]  (PORT required; identify the board by MAC, see CLAUDE.md)")

port = sys.argv[1]
cmd = sys.argv[2] if len(sys.argv) > 2 else "a"

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 1
s.dtr = False
s.rts = False
s.open()

time.sleep(0.5)
s.reset_input_buffer()
s.write((cmd + "\n").encode())
s.flush()

deadline = time.time() + 30
ok = False
while time.time() < deadline:
    line = s.readline().decode(errors="replace").rstrip()
    if line.startswith("[perf]"):
        print(line)
    if "done" in line:
        ok = True
        break
s.close()
sys.exit(0 if ok else 2)
