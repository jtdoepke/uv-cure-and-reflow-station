"""PlatformIO monitor filter: send STATUS, print the parsed reply, exit.

Adapted from mccahan/esp32-display-claude-base. Makes the serial monitor terminate on
its own so an agent's command never hangs:

    pio device monitor -e esp32dev_cyd_uidev -f get_status
"""

import os
import sys
import time

from platformio.public import DeviceMonitorFilterBase


class GetStatus(DeviceMonitorFilterBase):
    NAME = "get_status"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._buffer = ""
        self._in_status = False
        self._status_lines = []
        self._command_sent = False
        self._start_time = time.time()

    def rx(self, text):
        if not self._command_sent:
            self._command_sent = True
            if hasattr(self, "console") and hasattr(self.console, "serial"):
                self.console.serial.write(b"STATUS\n")

        self._buffer += text

        if "---STATUS_BEGIN---" in self._buffer:
            self._in_status = True
            self._buffer = self._buffer.split("---STATUS_BEGIN---", 1)[1]

        if self._in_status:
            while "\n" in self._buffer:
                line, self._buffer = self._buffer.split("\n", 1)
                line = line.strip()
                if line == "---STATUS_END---":
                    self.print_status()
                    os._exit(0)
                elif line:
                    self._status_lines.append(line)

        if time.time() - self._start_time > 10:
            sys.stderr.write("\nTimeout waiting for status response\n")
            os._exit(1)

        return text

    def tx(self, text):
        if not self._command_sent:
            self._command_sent = True
            return "STATUS\n" + text
        return text

    def print_status(self):
        print("\n" + "=" * 40)
        print("Device Status")
        print("=" * 40)
        for line in self._status_lines:
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            if key == "WIFI":
                print(f"WiFi Status: {value.replace('_', ' ').title()}")
            elif key == "IP":
                print(f"IP Address:  {value}")
            elif key == "SSID":
                print(f"Network:     {value}")
            elif key == "HEAP":
                print(f"Free Heap:   {int(value) // 1024} KB")
            elif key == "UPTIME":
                print(f"Uptime:      {value}s")
        print("=" * 40 + "\n")
