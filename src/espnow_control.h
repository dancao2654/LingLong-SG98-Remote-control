#pragma once

#include <Arduino.h>

bool espnow_control_start();
void espnow_control_stop();
void espnow_control_poll();
bool espnow_control_is_active();
uint8_t espnow_control_channel();
String espnow_control_status_text();
