#pragma once

#include <stdint.h>

void remote_pressure_ble_begin();
void remote_pressure_ble_end();
void remote_pressure_ble_service(uint16_t pressureCentiBar, int8_t batteryPercent, uint32_t nowMs);
bool remote_pressure_ble_ready();
bool remote_pressure_ble_connected();
bool remote_pressure_ble_streaming();
