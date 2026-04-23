#pragma once

// Older revisions of this project shipped a stub copy of this header so
// Arduino_GFX could build against toolchains that did not yet provide the real
// pin-manager API. Newer Arduino-ESP32 cores need the actual declarations for
// ESP_I2S, so forward to the framework header instead of shadowing it.
#if defined(__GNUC__)
#include_next <esp32-hal-periman.h>
#endif
