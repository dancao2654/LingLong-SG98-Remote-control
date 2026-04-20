#ifndef BLE_CONSOLE_H
#define BLE_CONSOLE_H

#include <Arduino.h>

bool ble_console_init();
bool ble_console_has_client();
void ble_console_send_line(const char* line);
void ble_console_set_advertising_enabled(bool enabled);
bool ble_console_advertising_enabled();
bool ble_console_take_dump_request();

const char* ble_console_device_name();
const char* ble_console_service_uuid();
const char* ble_console_tx_uuid();
const char* ble_console_rx_uuid();

#endif
