#pragma once

#include <Arduino.h>

bool espnow_control_start(bool coexistWithWifi = false);
void espnow_control_stop();
void espnow_control_poll();
void espnow_control_poll_probe_status();
bool espnow_control_broadcast_wifi_config(const String& ssid, const String& password);
bool espnow_control_push_status_if_due(uint32_t gaugeReadChangeSequence);
void espnow_control_set_wifi_client_active(bool active);
bool espnow_control_is_active();
bool espnow_control_is_wifi_client_active();
uint8_t espnow_control_channel();
unsigned long espnow_control_last_rx_at();
unsigned long espnow_control_last_command_rx_at();
unsigned long espnow_control_last_recovery_request_at();
String espnow_control_status_text();
