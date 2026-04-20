#pragma once

#include <Arduino.h>

void wifi_ui_init();
void wifi_ui_poll();
bool wifi_ui_sta_connected();
String wifi_ui_sta_ip();
String wifi_ui_lcd_status();
void wifi_ui_reset_network_settings();
