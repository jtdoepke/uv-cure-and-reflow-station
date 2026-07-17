// Oven-controller firmware entry point (esp32dev_control env; design.md §2, §11).
//
// Thin glue only: this file owns the Esp32* adapters and injects them into the portable logic
// in lib/control_logic + lib/protocol. The object graph below is deliberately the same one
// test_reliability_integration and test_safety_supervisor build over a LoopbackPipe with a
// FakeClock — the injection site is the only divergence between this target and the host tests
// (§11). If the two ever drift, the tests stop testing the firmware.
//
// The bench build (CONTROL_BENCH, env esp32dev_control_bench) closes §8 step 1: LEDs stand in
// for the heater SSR and the mains contactor, and pulling the CYD's TX or rebooting it must cut
// them within the 750 ms command-timeout. See control_board.h for why the link UART moves.
#include <Arduino.h>

#include "control_board.h"   // this board's pins, timeouts, link UART + console policy (§2/§6/§25)
#include "controller_link.h" // reliability facade (lib/control_logic)
#include "esp32_clock.h"
#include "esp32_contactor.h"
#include "esp32_heater_switch.h"
#include "esp32_serial_transport.h"
#include "esp32_watchdog.h"
#include "fault_sender.h"
#include "frame_link.h"
#include "heater_actuator.h"
#include "link_params.h"
#include "message_router.h"
#include "safety_supervisor.h"
#include "schema.h"             // shared wire-contract identity (lib/protocol)
#include "stub_thermocouples.h" // placeholder high-limit sensor until D4's TC adapter lands
#include "telemetry_sender.h"

// --- Link stack (§9) ---
// The MessageRouter is default-constructed and bound in setup(): FrameLink binds its handler at
// construction, but ControllerLink needs the FrameLink in order to send, so the cycle is broken
// with setObserver() (message_router.h). Forgetting that call drops every frame *silently*.
static Esp32Clock g_clk;
static Esp32SerialTransport g_link_uart(linkSerial());
static protocol::MessageRouter g_router;
static protocol::FrameLink g_link(g_link_uart, TF_SLAVE, g_router); // controller = TF_SLAVE
static ControllerLink g_ctrl(g_link, g_clk);

// Telemetry back to the CYD (§9), emitted unconditionally from boot — run or no run. Beyond
// feeding the live graph later, this stream is the CYD's *only* evidence we exist: its heartbeat
// proves the CYD is alive to us, and nothing proves the reverse, so without this the CYD cannot
// tell a healthy controller from an unplugged one. Today the payload is an otherwise-zeroed IDLE
// frame; A5/A6/D4 fill in temps, setpoint and duty as those land.
static protocol::TelemetrySender g_telemetry(g_link, g_clk);

// Fault annunciation to the CYD (§22): fires the dedicated Fault frame on change and re-sends
// while a fault stays active. Driven from SafetySupervisor::faultCode() each loop; the
// continuous telemetry.fault_code below is the self-healing backup channel.
static protocol::FaultSender g_fault(g_link, g_clk);

// --- Outputs (§4) ---
// Declared above the supervisor that owns them: its constructor drives the fail-safe state, and
// statics in one translation unit initialize in declaration order.
static Esp32HeaterSwitch g_heater_sw(kHeaterPin);
static Esp32Contactor g_contactor(kContactorPin);
static HeaterActuator g_heater(g_heater_sw, g_clk);
// High-limit input for the L3 over-temp / stuck-heater checks (§4). Stubbed (reports no
// usable channel) until D4's real TC adapter; the L3 checks only run once a run is armed,
// which rides in with D4. See stub_thermocouples.h.
static StubThermocouples g_tc;
static SafetySupervisor g_safety(g_ctrl, g_heater, g_contactor, g_tc, g_clk);

static Esp32Watchdog g_wdt;

namespace {

#if defined(CONTROL_BENCH)
const char *causeName(ResetCause c) {
  switch (c) {
  case ResetCause::PowerOn:
    return "PowerOn";
  case ResetCause::Watchdog:
    return "Watchdog";
  case ResetCause::Panic:
    return "Panic";
  case ResetCause::Software:
    return "Software";
  case ResetCause::Brownout:
    return "Brownout";
  default:
    return "Other";
  }
}
#endif

} // namespace

void setup() {
  // Outputs to their fail-safe default before anything else can run — §8 step 1's "outputs
  // default OFF". SafetySupervisor's constructor commanded this already, but it ran during static
  // init, where a digitalWrite cannot land: the pins are not outputs yet, and the adapters
  // deliberately drop writes made before begin(). Until this line the hardware pull-downs are the
  // whole guarantee — which is the point of them. These calls are what make it firmware's.
  g_heater_sw.begin();
  g_contactor.begin();

  g_wdt.begin(kWatchdogTimeoutMs);
  // Map the previous boot's reset cause onto a boot fault: a watchdog reset is reported once
  // as Fault{WATCHDOG} (§9) rather than the pair coming back silently. Read after begin().
  g_safety.noteResetCause(g_wdt.lastResetCause());

#if defined(CONTROL_BENCH)
  Serial.begin(115200); // bench only: UART0 is the console, the link is on UART2
  Serial.println();
  Serial.printf("[control] boot (BENCH: link UART2 rx=%d tx=%d, LED dummy load)\n", kLinkRxPin,
                kLinkTxPin);
  CONTROL_LOGF("[control] reset cause: %s (raw=%d)\n", causeName(g_wdt.lastResetCause()),
               (int)g_wdt.rawResetReason());
  // Two %08lx halves: 32-bit printf has no portable 64-bit format here.
  CONTROL_LOGF("[protocol] ver=%u schema=%08lx%08lx\n", (unsigned)protocol::kProtoVer,
               (unsigned long)(protocol::kSchemaHash >> 32),
               (unsigned long)(protocol::kSchemaHash & 0xFFFFFFFFu));
#endif

  // Buffer sizes must precede begin(). The controller *receives* Recipes, so RX is the side
  // that needs the headroom; see esp32_serial_transport.h on why TX must never short-write.
  linkSerial().setRxBufferSize(kLinkRxBuf);
  linkSerial().setTxBufferSize(kLinkTxBuf);
  linkSerial().begin(kLinkBaud, SERIAL_8N1, kLinkRxPin, kLinkTxPin);

  g_router.setObserver(g_ctrl); // without this every frame is dropped, silently
  // Hello; service() retransmits until the CYD answers. The nonce must be fresh every
  // boot: it is how the CYD spots that we restarted (watchdog/brownout/crash) and
  // re-announces itself, so the link comes back rather than sitting unmatched (§9).
  g_ctrl.begin(esp_random());
  CONTROL_LOGF("[control] boot nonce=%08lx\n", (unsigned long)g_ctrl.handshake().bootNonce());
}

void loop() {
  const uint32_t now = g_clk.millis();

  // Neither ControllerLink::service() nor CydLink::service() pumps the FrameLink — each
  // firmware's loop owns poll()/tick() (frame_link.h).
  g_link.poll();
  g_ctrl.service();

  static uint32_t last_tick_ms = 0;
  if (static_cast<uint32_t>(now - last_tick_ms) >= protocol::kLinkTickMs) {
    last_tick_ms = now;
    g_link.tick();
  }

#if defined(CONTROL_BENCH)
  // Bench dummy load — NOT production behavior. SafetySupervisor only ever *cuts*; it never
  // enables, and A5's PID (which will own duty) doesn't exist yet, so without this the heater
  // LED could never light and the fail-safe proof would have nothing to observe. At
  // windowMs=1000 a 0.5 duty is a 500 ms blink, which reads as "authorized AND the loop is
  // alive" — a solid LED would not distinguish those. The contactor LED, driven by the
  // supervisor itself, is the true authorized() readout. Delete when A5 lands.
  g_heater.setDuty(g_ctrl.authorized() ? 0.5F : 0.0F);

  // 'h' on the console hangs the loop on purpose, so §8 step 1's watchdog clause can be
  // observed rather than assumed: the outputs must go dark through the reset (hardware
  // pull-downs, no firmware involved) and the next boot must report ResetCause::Watchdog.
  if (Serial.available() > 0 && Serial.read() == 'h') {
    CONTROL_LOGF("[bench] hanging the loop on purpose - the watchdog should reset us\n");
    for (;;) {
      // A bare for(;;){} has no side effects, is UB, and the optimizer may delete it, so the
      // wait must be observable. delay() also yields, hanging *this* task specifically — which
      // is exactly the task the watchdog is subscribed to.
      delay(1000);
    }
  }
#endif

  g_heater.tick();
  g_safety.tick(); // LAST: safety has the final word over whatever duty was commanded (§4)
  g_wdt.kick();    // only after a full pass: this is what proves the loop got here

  // Report what the outputs actually ended up doing, so telemetry reflects the post-safety
  // truth rather than what the control loop asked for. session 0 = IDLE telemetry (§9): we are
  // stateless across a reset, so an unadopted session is exactly what a fresh boot should say.
  const uint32_t session = g_ctrl.gate().hasActiveSession() ? g_ctrl.gate().activeSession() : 0U;
  const oven_FaultCode fault = g_safety.faultCode();
  g_telemetry.state().heater_duty = g_heater.duty();
  g_telemetry.state().fault_code = fault; // continuous backup for the §22 annunciation
  g_telemetry.setSession(session);
  g_telemetry.service();

  // Dedicated Fault frame: fires on change, re-sends while active (§22). The supervisor has
  // already safed the outputs; this is the CYD-facing annunciation.
  g_fault.setSession(session);
  g_fault.set(fault);
  g_fault.service();

#if defined(CONTROL_BENCH)
  // Edge-triggered, so the fail-safe cut timestamps itself: the 1 Hz trace below is far too
  // coarse to tell a 750 ms command-timeout from a lucky guess, and §8 step 1's claim is about
  // *how fast* the output dies, not merely that it does.
  static bool last_auth = false;
  static bool last_safe = true;
  const bool auth_now = g_ctrl.authorized();
  if (auth_now != last_auth) {
    last_auth = auth_now;
    CONTROL_LOGF("[bench] authorized -> %d @ %lu ms\n", (int)auth_now, (unsigned long)now);
  }
  if (g_safety.safe() != last_safe) {
    last_safe = g_safety.safe();
    CONTROL_LOGF("[bench] safe -> %d @ %lu ms\n", (int)last_safe, (unsigned long)now);
  }
#endif

  static uint32_t last_log_ms = 0;
  if (static_cast<uint32_t>(now - last_log_ms) >= 1000U) {
    last_log_ms = now;
    CONTROL_LOGF("[control] matched=%d authorized=%d safe=%d\n", (int)g_ctrl.handshake().matched(),
                 (int)g_ctrl.authorized(), (int)g_safety.safe());
  }
  delay(10);
}
