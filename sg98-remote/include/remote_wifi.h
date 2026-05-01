#pragma once

#include <Arduino.h>

void remote_wifi_init();
bool remote_wifi_start();
void remote_wifi_stop();
void remote_wifi_poll();
bool remote_wifi_is_active();
bool remote_wifi_sta_connected();
bool remote_wifi_ap_running();
uint8_t remote_wifi_ap_station_count();
bool remote_wifi_has_saved_network();
bool remote_wifi_sta_connecting();
uint32_t remote_wifi_sta_attempt_count();
uint8_t remote_wifi_sta_disconnect_reason();
const char* remote_wifi_sta_disconnect_reason_text();
const char* remote_wifi_sta_last_event();
size_t remote_wifi_sta_password_length();
void remote_wifi_apply_network_settings(const String& ssid, const String& password, bool connectNow);
String remote_wifi_sta_ssid();
String remote_wifi_sta_ip();
String remote_wifi_ap_ssid();
String remote_wifi_ap_ip();
String remote_wifi_status_text();
void remote_wifi_reset_network_settings();
