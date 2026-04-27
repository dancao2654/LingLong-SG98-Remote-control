#pragma once

#include <Arduino.h>

void wifi_ui_init();
bool wifi_ui_start();
void wifi_ui_stop();
void wifi_ui_poll();
bool wifi_ui_is_active();
bool wifi_ui_sta_connected();
String wifi_ui_sta_ip();
String wifi_ui_lcd_status();
bool wifi_ui_setup_complete();
bool wifi_ui_has_saved_network();
uint8_t wifi_ui_ap_station_count();
bool wifi_ui_web_activity_recent();
void wifi_ui_reset_network_settings();
