// The C++ half of unity_config.h's Serial transport: Unity's runner is C and cannot name
// `Serial`, so these extern "C" shims are the only place the two meet. See unity_config.h.
//
// Lives at the test/ root, which PlatformIO compiles into every suite. Empty for the native
// envs — they have no Arduino and Unity's default stdout output is already right there.
#ifdef ARDUINO

#include <Arduino.h>

extern "C" {

void unity_output_start(void) {
  Serial.begin(115200);
}

void unity_output_char(char c) {
  Serial.write(c);
}

void unity_output_flush(void) {
  Serial.flush();
}

void unity_output_complete(void) {
  Serial.end();
}

} // extern "C"

#endif // ARDUINO
