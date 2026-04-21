#pragma once

#include <Arduino.h>

enum RadioMode : uint8_t {
    RADIO_MODE_WEB = 0,
    RADIO_MODE_ESPNOW = 1
};

void radio_mode_init();
void radio_mode_poll();
bool radio_mode_apply(bool force = false);
bool radio_mode_can_switch_now();
RadioMode radio_mode_current();
RadioMode radio_mode_desired();
bool radio_mode_is_web();
bool radio_mode_is_espnow();
const char* radio_mode_label(RadioMode mode);
bool radio_mode_status_ok();
String radio_mode_lcd_status();
String radio_mode_status_text();
