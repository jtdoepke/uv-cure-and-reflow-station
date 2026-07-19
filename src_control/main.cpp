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
#include <LittleFS.h>

#include "control_board.h"   // this board's pins, timeouts, link UART + console policy (§2/§6/§25)
#include "controller_link.h" // reliability facade (lib/control_logic)
#include "device_settings.h" // control::SettingsStore (§4/§7/§24; Wave R2b)
#include "esp32_clock.h"
#include "littlefs_profile_storage.h"  // IProfileStorage adapter (§7; moved here in Wave R2)
#include "littlefs_settings_storage.h" // ISettingsStorage adapter (§7/§24; Wave R2b)
#include "management_responder.h"      // profile/settings management responder (§9; Wave R2)
#include "profile_library.h"           // control::ProfileStore (§7/§23; Wave R2)
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
#include "esp32_door_sensor.h"  // IDoorSensor adapter — the donor's DS3 dry contact (§4/§15)
#include "telemetry_sender.h"

#if defined(CONTROL_SIM)
// A10 bench plant simulator (env esp32dev_control_sim): closes the control loop against a synthetic
// oven so the real firmware run path is exercised before mains hardware (D3/D4) exists.
#include "heater_control.h" // A5 PID (run path)
#include "oven_cal.h"       // kDefaultModel — the feedforward model, physics-anchored to the plant
#include "oven_plant.h"     // A10 thermal-plant twin
#include "profile_executor.h"  // A6 run engine (driven by the link, ticked by the run path)
#include "run_path.h"          // ControllerRunPath — executor + PID composition
#include "sim_thermocouples.h" // plant-backed IThermocouples (replaces the stub)
#endif

// Bigger loopTask stack: the profile library added a deep call chain with large stack locals —
// control::ProfileStore::list() holds ProfileEntry[32] (~1 KB) + Summary[32] (~1.4 KB) and calls
// loadBlob() which nests a kBlobCap (~1.5 KB) decode buffer, and the responder's reply path adds an
// oven_ProfileList/Data (~1.5 KB) buffer. The 8 kB default overflowed on the first list() (a
// hardware-only crash: Guru Meditation LoadStoreAlignment + "Stack canary (loopTask)", the boot
// bench-log listing the seeds; host tests have no such limit). 16 kB gives headroom and costs 8 kB
// of the controller's ample internal heap. Mirrors src_cyd/main.cpp's identical lesson.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

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

// --- Profile library + settings store (§7/§23; Wave R2 of the §2 "CYD is a UI remote" split) ---
// The profile library moved off the CYD to here: one LittleFS directory per mode, one store each,
// answered over the link by the ManagementResponder. The CYD is now a remote client (R3).
static LittleFsProfileStorage g_cure_fs("/profiles/cure");
static LittleFsProfileStorage g_reflow_fs("/profiles/reflow");
static control::ProfileStore g_cure_store(g_cure_fs, oven_Mode_MODE_CURE);
static control::ProfileStore g_reflow_store(g_reflow_fs, oven_Mode_MODE_REFLOW);
static LittleFsSettingsStorage g_settings_fs;
static control::SettingsStore g_settings(g_settings_fs);
static ManagementResponder g_mgmt(g_link, g_cure_store, g_reflow_store);

// --- Outputs (§4) ---
// Declared above the supervisor that owns them: its constructor drives the fail-safe state, and
// statics in one translation unit initialize in declaration order.
static Esp32HeaterSwitch g_heater_sw(kHeaterPin);
static Esp32Contactor g_contactor(kContactorPin);
static HeaterActuator g_heater(g_heater_sw, g_clk);

#if defined(CONTROL_SIM)
// The bench simulator's temperature source: the OvenPlant turns commanded outputs into
// temperatures, and SimThermocouples feeds them back through the real IThermocouples port in place
// of the stub. The executor (A6) + PID (A5) are the same units the production loop will run once
// D4's real TC adapter lands — composed here by ControllerRunPath. NOT production; fabricates
// readings, so never at a real oven.
static ProfileExecutor g_exec(g_clk);
static HeaterControl g_pid(g_clk);
static OvenPlant g_plant;
static SimThermocouples g_tc(g_plant);
#else
// High-limit input for the L3 over-temp / stuck-heater checks (§4). Stubbed (reports no
// usable channel) until D4's real TC adapter; the L3 checks only run once a run is armed,
// which rides in with D4. See stub_thermocouples.h.
static StubThermocouples g_tc;
#endif
// Door sense (§4/§6/§15). Declared UNCONDITIONALLY — unlike the plant, this is production wiring:
// a real GPIO reading the donor's DS3 dry contact, the same posture A8 took with the contactor. On
// the bench a jumper to GND stands in for DS3. Nothing here gates power; the DS1 interlock does
// that in hardware (§4 L0), and this only tells the firmware what happened.
static Esp32DoorSensor g_door(kDoorPin, kDoorDebounceMs);

static SafetySupervisor g_safety(g_ctrl, g_heater, g_contactor, g_tc, g_clk);
#if defined(CONTROL_SIM)
// Declared after the supervisor it references. The link (setExecutor below) owns the executor's
// load/start/abort lifecycle; the run path ticks it each loop with the measured control temp,
// drives the PID, and arms/disarms the supervisor's L3 checks around the run.
static ControllerRunPath g_runpath(g_exec, g_pid, g_safety, g_heater, g_ctrl, g_tc, g_door,
                                   oven_cal::kDefaultModel);
#endif

static Esp32Watchdog g_wdt;

namespace {

#if defined(CONTROL_BENCH) || defined(CONTROL_SIM)
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
  g_door.begin(); // seeds the debounce from the pin's actual level — booting open reports open

  g_wdt.begin(kWatchdogTimeoutMs);
  // Map the previous boot's reset cause onto a boot fault: a watchdog reset is reported once
  // as Fault{WATCHDOG} (§9) rather than the pair coming back silently. Read after begin().
  g_safety.noteResetCause(g_wdt.lastResetCause());

  // Mount the profile-library filesystem (§7; Wave R2). formatOnFail so a fresh/blank board comes
  // up with an empty library rather than a mount error; the stock seed set is `uploadfs`-flashed
  // into /profiles/<mode>/. A mount failure is non-fatal to the safety loop — the library is just
  // empty. The stores are already constructed (their dirs are created lazily on first write).
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    CONTROL_LOGF("[profiles] LittleFS mount failed - library unavailable\n");
  }
  g_settings.load(); // persisted settings (or defaults), caps re-clamped to hard-max (§4)

#if defined(CONTROL_BENCH) || defined(CONTROL_SIM)
  Serial.begin(115200); // bench/sim only: UART0 is the console, the link is on UART2
  Serial.println();
#if defined(CONTROL_SIM)
  Serial.printf("[control] boot (SIM: link UART2 rx=%d tx=%d, plant simulator)\n", kLinkRxPin,
                kLinkTxPin);
#else
  Serial.printf("[control] boot (BENCH: link UART2 rx=%d tx=%d, LED dummy load)\n", kLinkRxPin,
                kLinkTxPin);
#endif
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

  g_router.setObserver(g_ctrl);          // without this every frame is dropped, silently
  g_mgmt.setSettingsStore(g_settings);   // answer SettingsGet/Put too (§9, R2b)
  g_ctrl.setManagementResponder(g_mgmt); // route the CYD's profile-management requests (§9, R2)

#if defined(CONTROL_SIM)
  // Let the link drive the executor off the accepted Recipe/Start/Abort (load/start/abort); the run
  // path ticks it each loop. Without this the CYD's bench stimulus would authorize the link but no
  // run would ever begin.
  g_ctrl.setExecutor(g_exec);
#endif

#if defined(CONTROL_BENCH) || defined(CONTROL_SIM)
  {
    control::ProfileStore::Summary rows[control::ProfileStore::kMaxListed];
    const size_t nc = g_cure_store.list(rows, control::ProfileStore::kMaxListed);
    const size_t nr = g_reflow_store.list(rows, control::ProfileStore::kMaxListed);
    CONTROL_LOGF("[profiles] cure=%u reflow=%u\n", (unsigned)nc, (unsigned)nr);
  }
#endif
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

#if defined(CONTROL_SIM)
  // Closed loop against the plant: read the sim thermocouples, advance the executor + PID, command
  // the resulting duty, and arm/disarm the L3 checks. Runs before the actuator/safety ticks so
  // safety still has the final word (below).
  g_runpath.tick();
#endif

  g_heater.tick();
  g_safety.tick(); // LAST: safety has the final word over whatever duty was commanded (§4)

#if defined(CONTROL_SIM)
  // Integrate the plant over this loop interval with the POST-safety applied duty — what the heater
  // actually did (safety may have forced it off) — plus the executor's channel states. One-loop-
  // delayed feedback; the next iteration reads the updated temps. Real-time on-device (~10 ms
  // loop); the host sim advances the clock faster.
  {
    static uint32_t sim_last_ms = 0;
    const float dtS = sim_last_ms == 0 ? 0.0F : static_cast<float>(now - sim_last_ms) / 1000.0F;
    sim_last_ms = now;
    const ProfileExecutor::Output &so = g_runpath.output();
    // The door is handed to the PLANT, not just reported: DS1 sits in the element's line
    // conductor, so an open door removes heater power in hardware whatever duty was commanded.
    g_plant.step(dtS, g_heater.duty(), so.convFan, so.uv, so.motor, g_door.isOpen());
  }
#endif

  g_wdt.kick(); // only after a full pass: this is what proves the loop got here

  // Report what the outputs actually ended up doing, so telemetry reflects the post-safety
  // truth rather than what the control loop asked for. session 0 = IDLE telemetry (§9): we are
  // stateless across a reset, so an unadopted session is exactly what a fresh boot should say.
  const uint32_t session = g_ctrl.gate().hasActiveSession() ? g_ctrl.gate().activeSession() : 0U;
  const oven_FaultCode fault = g_safety.faultCode();
  g_telemetry.state().heater_duty = g_heater.duty();
  g_telemetry.state().fault_code = fault; // continuous backup for the §22 annunciation
  const bool door_open = g_door.isOpen();
  g_telemetry.state().door_open = door_open;

#if defined(CONTROL_SIM)
  // Fill the live telemetry the CYD's Home screen renders (§14): the sim thermocouples' wall +
  // workpiece readings and the run path's setpoint / channels / run state. Non-sim builds leave
  // these zero (StubThermocouples reports no channel) until D4's real TC adapter lands. Faulted
  // channels are omitted so a stray 0 can't masquerade as a reading.
  {
    oven_Telemetry &ts = g_telemetry.state();
    constexpr pb_size_t kMaxWall = sizeof(ts.wall_temp) / sizeof(ts.wall_temp[0]);
    ts.wall_temp_count = 0;
    const int nw = g_tc.wallCount();
    for (int i = 0; i < nw && ts.wall_temp_count < kMaxWall; ++i) {
      const TcReading r = g_tc.wall(i);
      if (!r.fault) {
        ts.wall_temp[ts.wall_temp_count++] = r.celsius;
      }
    }
    const TcReading wp = g_tc.workpiece();
    ts.work_temp = wp.fault ? 0.0F : wp.celsius;
    const ProfileExecutor::Output &o = g_runpath.output();
    ts.setpoint = o.setpointC;
    ts.run_state = o.runState;
    ts.seg_idx = o.segIdx;
    ts.elapsed_ms = o.elapsedMs; // §15: the CYD's ETA/progress + projection alignment
    ts.conv_fan = o.convFan;
    ts.uv_duty = o.uv ? 1.0F : 0.0F;
    ts.motor = o.motor;
  }
#endif
  g_telemetry.setSession(session);
  // §9: "an immediate unsolicited telemetry on doorOpen change". The 250 ms cadence is fine for a
  // temperature graph but not for the things the door gates — waking the display (§17), ending the
  // run page (§15), enabling Start (§19) — where a quarter second of the UI disagreeing with the
  // physical machine is exactly the lag an operator reads as the thing being broken.
  static bool door_open_prev = false;
  static bool door_seen = false;
  if (!door_seen || door_open != door_open_prev) {
    door_seen = true;
    door_open_prev = door_open;
    g_telemetry.sendNow();
  } else {
    g_telemetry.service();
  }

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
#if defined(CONTROL_SIM)
    // The simulated trajectory, so the bench operator can watch a run ramp/soak/peak/coast.
    const ProfileExecutor::Output &lo = g_runpath.output();
    CONTROL_LOGF("[sim] state=%d seg=%u elapsed=%us sp=%.1f wall=%.1f work=%.1f bay=%.1f duty=%.2f "
                 "fault=%d\n",
                 (int)lo.runState, (unsigned)lo.segIdx, (unsigned)(lo.elapsedMs / 1000U),
                 lo.setpointC, g_plant.wallTempC(), g_plant.workpieceTempC(), g_plant.bayTempC(),
                 g_heater.duty(), (int)g_safety.faultCode());
#endif
  }
  delay(10);
}
