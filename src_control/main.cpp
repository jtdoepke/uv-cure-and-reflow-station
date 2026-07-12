// Oven-controller firmware entry point (esp32dev_control env; design.md §2, §11).
//
// This tree is the controller counterpart of src_cyd/: thin glue only. It owns
// the Esp32* adapters and injects them into the portable logic in
// lib/control_logic; decisions live behind the lib/control_port interfaces so
// they stay host-testable in the native_control env.
#include <Arduino.h>

#include "IClock.h"
#include "heartbeat_monitor.h"

namespace {

// Adapter: IClock over Arduino millis().
class Esp32Clock : public IClock {
public:
  uint32_t millis() override { return ::millis(); }
};

Esp32Clock clk;
HeartbeatMonitor heartbeat(clk);

} // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("[control] boot");
}

void loop() {
  // Skeleton heartbeat-status log (~1 Hz). The real link (lib/protocol, backlog
  // A1/A2) will feed() the monitor from Heartbeat frames; until then it reports
  // expired, which is the correct fail-safe reading of "no CYD attached".
  static uint32_t last_log_ms = 0;
  uint32_t now = clk.millis();
  if (now - last_log_ms >= 1000) {
    last_log_ms = now;
    Serial.printf("[control] alive, heartbeat %s\n",
                  heartbeat.expired(750) ? "EXPIRED (no link)" : "ok");
  }
  delay(10);
}
