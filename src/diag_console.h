#pragma once

#include <Arduino.h>

void diag_console_init();
void diag_console_poll();

String diag_api_status_json();
String diag_api_snapshot_json();
bool diag_api_set_menu_value(uint8_t menuNum, uint8_t itemIdx, int32_t value, String* message = nullptr);
bool diag_api_set_rpm(int32_t value, String* message = nullptr);
bool diag_api_set_weight(int32_t value, String* message = nullptr);
bool diag_api_run_action(const char* action, const char* arg0 = nullptr, const char* arg1 = nullptr, String* message = nullptr);
