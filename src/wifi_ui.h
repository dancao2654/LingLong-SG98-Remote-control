#pragma once

#include <Arduino.h>

void wifi_ui_init();
bool wifi_ui_start();
void wifi_ui_stop();
void wifi_ui_poll();
bool wifi_ui_is_active();
bool wifi_ui_sta_connected();
bool wifi_ui_ap_running();
String wifi_ui_sta_ip();
String wifi_ui_sta_ssid();
int32_t wifi_ui_sta_rssi();
String wifi_ui_sta_status();
String wifi_ui_lcd_status();
bool wifi_ui_setup_complete();
bool wifi_ui_has_saved_network();
bool wifi_ui_home_setup_online();
void wifi_ui_set_captive_setup_complete(bool complete);
uint8_t wifi_ui_ap_station_count();
bool wifi_ui_web_activity_recent();
bool wifi_ui_web_idle_for(unsigned long quietMs);
bool wifi_ui_save_network_settings(const String& ssid, const String& password);
void wifi_ui_reset_network_settings();
String wifi_ui_status_json();
String wifi_ui_scan_json(bool force);
