"""PlatformIO extra script for the esp32dev_cyd_uidev env.

Adapted from mccahan/esp32-display-claude-base (extra_script.py):
- after `pio run -e esp32dev_cyd_uidev -t upload`, waits for boot and prints the device's
  STATUS block (including its IP) so the agent never has to ask for it;
- registers `pio run -e esp32dev_cyd_uidev -t status` to re-query without flashing.
Both exit cleanly — no hanging monitors.
"""

import json
import time

Import("env")  # noqa: F821  (PlatformIO SCons construct)


def _find_port(env):
    import serial.tools.list_ports

    upload_port = env.subst("$UPLOAD_PORT")
    if upload_port:
        return upload_port
    for port in serial.tools.list_ports.comports():
        dev = port.device.lower()
        if "ttyusb" in dev or "ttyacm" in dev or "usbserial" in dev:
            return port.device
    return None


def _query_status(source, target, env, wait_boot=True):
    import serial

    port = _find_port(env)
    if not port:
        print("Error: could not find a serial port (is the board on Micro-USB?)")
        return

    if wait_boot:
        print("\nWaiting for device to boot...")
        time.sleep(3)

    print(f"Querying device on {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.write(b"STATUS\n")

        deadline = time.time() + 10
        buffer = ""
        in_status = False
        status = {}
        while time.time() < deadline:
            if ser.in_waiting:
                buffer += ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
                if "---STATUS_BEGIN---" in buffer:
                    in_status = True
                    buffer = buffer.split("---STATUS_BEGIN---", 1)[1]
                if in_status:
                    while "\n" in buffer:
                        line, buffer = buffer.split("\n", 1)
                        line = line.strip()
                        if line == "---STATUS_END---":
                            ser.close()
                            _print_status(status)
                            return
                        if ":" in line:
                            key, value = line.split(":", 1)
                            status[key] = value
            else:
                time.sleep(0.05)
        ser.close()
        print("Timeout waiting for status response")
    except Exception as exc:  # pragma: no cover - direct operator feedback
        print(f"Error: {exc}")


def _print_status(status):
    print("\n" + "=" * 40)
    print("Device Status")
    print("=" * 40)
    print(f"WiFi Status: {status.get('WIFI', 'Unknown').title()}")
    if "IP" in status:
        print(f"IP Address:  {status['IP']}")
    if "SSID" in status:
        print(f"Network:     {status['SSID']}")
    if "HEAP" in status:
        print(f"Free Heap:   {int(status['HEAP']) // 1024} KB")
    if "UPTIME" in status:
        print(f"Uptime:      {status['UPTIME']}s")
    print("=" * 40)
    json_status = {
        "wifi": status.get("WIFI", "UNKNOWN").lower(),
        "ip": status.get("IP", ""),
        "ssid": status.get("SSID", ""),
        "heap": int(status.get("HEAP", 0)),
        "uptime": int(status.get("UPTIME", 0)),
    }
    print(f"\nJSON: {json.dumps(json_status)}\n")


def _status_no_wait(source, target, env):
    _query_status(source, target, env, wait_boot=False)


env.AddPostAction("upload", _query_status)  # noqa: F821
env.AddCustomTarget(  # noqa: F821
    name="status",
    dependencies=None,
    actions=[env.VerboseAction(_status_no_wait, "Getting device status...")],  # noqa: F821
    title="Device Status",
    description="Query device IP/status via serial (no flash)",
)
