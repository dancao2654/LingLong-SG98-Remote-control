#pragma once

#include <Arduino.h>

bool remote_ws_start();
void remote_ws_stop();
void remote_ws_poll();
bool remote_ws_broadcast_status_if_due(uint32_t gaugeReadChangeSequence);
bool remote_ws_is_active();
