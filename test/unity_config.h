// Shared Unity transport config. PlatformIO auto-defines UNITY_INCLUDE_CONFIG_H when this
// file exists at the test/ root, so it's included by every suite (native and embedded).
//
// Native (no ARDUINO): leave Unity's defaults — output goes to stdout.
// Embedded (Arduino): route Unity output to the first Serial so `pio test -e embedded`
// can scrape results over USB.
#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#define UNITY_OUTPUT_CHAR(c) Serial.write((char)(c))
#define UNITY_OUTPUT_FLUSH() Serial.flush()
#define UNITY_OUTPUT_START() Serial.begin(115200)
#define UNITY_OUTPUT_COMPLETE() Serial.end()
#endif
