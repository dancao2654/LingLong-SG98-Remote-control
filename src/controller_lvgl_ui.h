#pragma once

#include <stdint.h>

class Adafruit_ST7789;

void controller_lvgl_init(Adafruit_ST7789* display, uint16_t width, uint16_t height);
void controller_lvgl_sync();
void controller_lvgl_set_enabled(bool enabled);
void controller_lvgl_set_output_enabled(bool enabled);
void controller_lvgl_invalidate();
bool controller_lvgl_ready();
void controller_lvgl_show_motor_probe_splash(const char* motorLabel);
void controller_lvgl_show_flash(const char* label, uint32_t durationMs);
