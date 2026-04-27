#pragma once

#include <Arduino.h>

enum RadioMode : uint8_t {
    RADIO_MODE_WEB = 0,
    RADIO_MODE_ESPNOW = 1,
    RADIO_MODE_OFF = 2,
    RADIO_MODE_DUAL = 3
};

void radio_mode_init();
void radio_mode_poll();
bool radio_mode_apply(bool force = false);
bool radio_mode_can_switch_now();
RadioMode radio_mode_current();
RadioMode radio_mode_desired();
RadioMode radio_mode_from_value(int32_t value);
int32_t radio_mode_sanitize_value(int32_t value);
bool radio_mode_is_web();
bool radio_mode_is_espnow();
bool radio_mode_is_off();
bool radio_mode_is_dual();
bool radio_mode_web_recovery_active();
unsigned long radio_mode_recovery_remaining_ms();
const char* radio_mode_label(RadioMode mode);
const char* radio_mode_value_label(int32_t value);
bool radio_mode_status_ok();
String radio_mode_lcd_status();
String radio_mode_status_text();
