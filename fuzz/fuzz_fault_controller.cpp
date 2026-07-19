// Internal-correctness harness: the §22 fault latch + table (B7, lib/app_logic/fault_controller.h
// + fault_table.h), whose only consumer is the one screen that must never fail — the fault modal
// that appears unbidden to tell the operator the machine has already been forced safe.
//
// Why this is a parsing-like seam despite being "internal": `oven_FaultCode` arrives from the wire.
// The generated enum is a C enum, so a corrupt-but-CRC-valid frame, a controller running a skewed
// schema, or a future firmware with a code this build has never heard of all hand us a value
// outside the enumerators. Every string the overlay draws is looked up by that value.
//
// Invariants:
//   totality   — formatTitle/guidanceText/codeNameText produce a usable string for ANY uint32_t
//                code. A null here is lv_label_set_text(nullptr) on the fault modal.
//   formatting — formatTitle always NUL-terminates AND leaves valid UTF-8, for every code x every
//                buffer size. It snprintf's into a caller-owned fixed buffer, and the operator copy
//                in this table is not guaranteed ASCII forever (kSafedGuidance already carries a
//                multi-byte `·`), so a truncation that splits a multi-byte sequence would draw a
//                broken glyph. ASAN covers the buffer itself; this covers the encoding.
//   latch      — under ANY sequence of raises / ticks / setRunActive, an active latch clears ONLY
//                via acknowledge() (§22: "never auto-dismiss ... even if the condition clears");
//                the displayed cause never DROPS in severity, which is what stops a LINK_LOST storm
//                masking a latched over-temp; overTemp is sticky across a supersede; and
//                acknowledge() never reports None while active.
//
// Input: [0] = op count seed, then one byte per step:
//   bits 0-1  op: 0 raise, 1 tick(healthy), 2 tick(unhealthy), 3 setRunActive toggle
//   bits 2-7  code selector (mapped over the known enumerators plus out-of-range values)
//
// NOTE the code type: fault_table takes the RAW WIRE int32_t, never `oven_FaultCode`. Writing this
// harness is what forced that change — the first version cast an out-of-range value to the enum to
// call the old API, and UBSAN rejected the cast on the very first corpus unit. The old signature
// could not be exercised with the inputs it was supposed to defend against, which was the bug.
#include <cstring>

#include "fuzz_util.h"

#include "fault_controller.h"
#include "fault_table.h"

namespace {

// The known enumerators plus deliberately out-of-range values — the codes a skewed peer can send.
// Kept as int32_t, never cast to oven_FaultCode: that cast is itself the UB this whole seam exists
// to avoid, and UBSAN flags it in the harness just as readily as in the firmware.
const fault_table::FaultCodeWire kCodes[] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    63,
    64,
    255,
    256,
    1000,
    9999,
    -1,
    -12345,
    2147483647,
    -2147483647 - 1,
};
constexpr size_t kCodeCount = sizeof(kCodes) / sizeof(kCodes[0]);

fault_table::FaultCodeWire codeAt(uint8_t sel) {
  return kCodes[sel % kCodeCount];
}

// Is `s` well-formed UTF-8? A truncated multi-byte sequence is the failure this catches.
bool validUtf8(const char *s) {
  const auto *p = reinterpret_cast<const unsigned char *>(s);
  while (*p != 0) {
    size_t extra = 0;
    if (*p < 0x80U) {
      extra = 0;
    } else if ((*p & 0xE0U) == 0xC0U) {
      extra = 1;
    } else if ((*p & 0xF0U) == 0xE0U) {
      extra = 2;
    } else if ((*p & 0xF8U) == 0xF0U) {
      extra = 3;
    } else {
      return false; // a stray continuation byte or an invalid lead
    }
    ++p;
    for (size_t i = 0; i < extra; ++i) {
      if ((*p & 0xC0U) != 0x80U) {
        return false; // sequence cut short (by truncation) or malformed
      }
      ++p;
    }
  }
  return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2) {
    return 0;
  }

  // --- Table totality + formatTitle encoding safety, across every code and every buffer size ---
  for (size_t i = 0; i < kCodeCount; ++i) {
    const fault_table::FaultCodeWire c = kCodes[i];
    FUZZ_ASSERT(fault_table::guidanceText(c) != nullptr);
    FUZZ_ASSERT(fault_table::codeNameText(c) != nullptr);
    // A recognized code must carry all three strings; that is what `known` promises.
    const fault_table::FaultInfo info = fault_table::faultInfo(c);
    if (info.known) {
      FUZZ_ASSERT(info.title != nullptr && info.guidance != nullptr && info.codeName != nullptr);
    }
    // Every buffer size from degenerate to ample. n == 0 must not write at all (ASAN would catch a
    // 1-byte NUL store into a zero-length buffer).
    for (size_t n = 0; n <= 72; ++n) {
      char buf[80];
      std::memset(buf, 0x7F, sizeof(buf));
      fault_table::formatTitle(c, buf, n);
      if (n == 0) {
        FUZZ_ASSERT(buf[0] == 0x7F);
        continue;
      }
      // Terminated within the promised bounds...
      bool terminated = false;
      for (size_t k = 0; k < n; ++k) {
        if (buf[k] == '\0') {
          terminated = true;
          break;
        }
      }
      FUZZ_ASSERT(terminated);
      // ...and never left holding half a multi-byte character.
      FUZZ_ASSERT(validUtf8(buf));
    }
    // A null destination must be a no-op, not a crash.
    fault_table::formatTitle(c, nullptr, 32);
  }

  // --- Latch invariants under an adversarial op sequence ---
  FaultController fc;
  uint32_t now = 0;
  bool everActive = false;
  uint8_t prevSeverity = 0;
  bool sawOverTemp = false;

  for (size_t i = 1; i < size; ++i) {
    const uint8_t b = data[i];
    now += 1 + (b & 0x0FU); // monotonic clock; the latch must not depend on the step size

    const FaultState before = fc.state();

    switch (b & 0x03U) {
    case 0:
      fc.onControllerFault(now, static_cast<uint32_t>(b), codeAt(static_cast<uint8_t>(b >> 2)));
      break;
    case 1:
      fc.tick(now, /*linkHealthy=*/true);
      break;
    case 2:
      fc.tick(now, /*linkHealthy=*/false);
      break;
    default:
      fc.setRunActive((b & 0x80U) != 0U);
      break;
    }

    const FaultState after = fc.state();

    // §22's headline rule. acknowledge() is the ONLY caller below that may clear, and it is not in
    // this loop — so nothing here may take an active latch down.
    FUZZ_ASSERT(!before.active || after.active);

    if (after.active) {
      everActive = true;
      // The cause never regresses to a lower severity within an episode: a storm of link losses
      // must not be able to push a latched over-temp off the screen.
      const uint8_t sev = static_cast<uint8_t>(after.severity);
      if (before.active) {
        FUZZ_ASSERT(sev >= prevSeverity);
      }
      prevSeverity = sev;
      // FAULT_NONE is "no fault" — a latch displaying it would render as a fault with no cause.
      FUZZ_ASSERT(after.code != oven_FaultCode_FAULT_NONE);
      // Whatever is latched, the overlay can draw it.
      FUZZ_ASSERT(fault_table::guidanceText(after.code) != nullptr);
      FUZZ_ASSERT(fault_table::codeNameText(after.code) != nullptr);
      FUZZ_ASSERT(after.count >= 1);
    }
    // overTemp is sticky for the episode (§14 HOT / §17 sleep suppression key off it), so a
    // later, higher-severity non-over-temp cause must not clear it.
    if (before.overTemp) {
      FUZZ_ASSERT(after.overTemp);
      sawOverTemp = true;
    }
  }

  // Acknowledge always works and always reports a real destination while active.
  const bool wasActive = fc.active();
  const bool overTempAtAck = fc.overTempLatched();
  const AckRoute route = fc.acknowledge();
  if (wasActive) {
    FUZZ_ASSERT(route == AckRoute::Home || route == AckRoute::RunSummary);
    FUZZ_ASSERT(!fc.active());
  } else {
    FUZZ_ASSERT(route == AckRoute::None);
  }
  // §22: acknowledge dismisses the ALARM, not the hazard — the over-temp latch survives it, which
  // is what keeps HOT on Home and sleep suppressed until the chamber cools.
  FUZZ_ASSERT(fc.overTempLatched() == overTempAtAck);
  fc.clearOverTemp();
  FUZZ_ASSERT(!fc.overTempLatched());

  (void)everActive;
  (void)sawOverTemp;
  return 0;
}
