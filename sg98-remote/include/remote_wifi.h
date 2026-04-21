#pragma once

#include <Arduino.h>

void remote_wifi_init();
bool remote_wifi_start();
void remote_wifi_stop();
void remote_wifi_poll();
bool remote_wifi_is_active();
bool remote_wifi_sta_connected();
bool remote_wifi_has_saved_network();
String remote_wifi_sta_ssid();
String remote_wifi_sta_ip();
String remote_wifi_ap_ssid();
String remote_wifi_ap_ip();
String remote_wifi_status_text();
void remote_wifi_reset_network_settings();
