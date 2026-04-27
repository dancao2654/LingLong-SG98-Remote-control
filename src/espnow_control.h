#pragma once

#include <Arduino.h>

bool espnow_control_start(bool coexistWithWifi = false);
void espnow_control_stop();
void espnow_control_poll();
bool espnow_control_broadcast_wifi_config(const String& ssid, const String& password);
void espnow_control_set_wifi_client_active(bool active);
bool espnow_control_is_active();
bool espnow_control_is_wifi_client_active();
uint8_t espnow_control_channel();
unsigned long espnow_control_last_rx_at();
unsigned long espnow_control_last_command_rx_at();
String espnow_control_status_text();
