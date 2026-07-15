// Shared Unity transport config. PlatformIO auto-defines UNITY_INCLUDE_CONFIG_H when this
// file exists at the test/ root, so it's included by every suite (native and embedded).
//
// Native (no ARDUINO): leave Unity's defaults — output goes to stdout.
// Embedded (Arduino): route Unity output to the first Serial so `pio test -e embedded`
// can scrape results over USB.
//
// The indirection through unity_output.cpp is load-bearing, not style: Unity is C, and `Serial`
// is a C++ object, so unity.c cannot name it. Calling Serial straight from these macros compiles
// the suite's own C++ TU fine and fails every C one with "'Serial' undeclared" — which is exactly
// how this env came to be broken. Guarding on __cplusplus instead would "fix" the build by
// silently dropping the serial output Unity needs to report results at all.
#pragma once

#ifdef ARDUINO

#ifdef __cplusplus
extern "C" {
#endif

// Implemented in test/unity_output.cpp — the C++ side, which may touch Serial.
void unity_output_start(void);
void unity_output_char(char c);
void unity_output_flush(void);
void unity_output_complete(void);

#ifdef __cplusplus
}
#endif

#define UNITY_OUTPUT_CHAR(c) unity_output_char((char)(c))
#define UNITY_OUTPUT_FLUSH() unity_output_flush()
#define UNITY_OUTPUT_START() unity_output_start()
#define UNITY_OUTPUT_COMPLETE() unity_output_complete()

#endif // ARDUINO
