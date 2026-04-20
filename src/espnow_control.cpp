#include "espnow_control.h"

#include <cstring>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "definitions.h"
#include "diag_console.h"
#include "display.h"
#include "main.h"

namespace {
constexpr size_t ESPNOW_COMMAND_LEN = 64;
constexpr uint8_t ESPNOW_RX_QUEUE_LEN = 8;

struct EspNowEvent {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    char payload[ESPNOW_COMMAND_LEN];
};

QueueHandle_t espnowQueue = nullptr;
bool espnowActive = false;
uint8_t espnowChannelValue = default_espnow_channel;
unsigned long lastRxAt = 0;
uint32_t rxCount = 0;
char lastCommand[ESPNOW_COMMAND_LEN] = "";

void onEspNowReceive(const uint8_t* mac_addr, const uint8_t* data, int data_len) {
    if(espnowQueue == nullptr || data == nullptr || data_len <= 0) {
        return;
    }

    EspNowEvent event = {};
    if(mac_addr != nullptr) {
        memcpy(event.mac, mac_addr, sizeof(event.mac));
    }

    const int copyLen = data_len < static_cast<int>(sizeof(event.payload) - 1) ? data_len : static_cast<int>(sizeof(event.payload) - 1);
    memcpy(event.payload, data, copyLen);
    event.payload[copyLen] = '\0';

    xQueueSend(espnowQueue, &event, 0);
}

bool parseIntArgument(const String& raw, const char* prefix, int32_t* value) {
    const String token = String(prefix) + "=";
    if(!raw.startsWith(token)) {
        return false;
    }

    String number = raw.substring(token.length());
    number.trim();
    if(number.length() == 0) {
        return false;
    }

    *value = number.toInt();
    return true;
}

bool runAction(const char* action, String* message) {
    return diag_api_run_action(action, nullptr, nullptr, message);
}

bool handleEspNowCommand(const char* payload, String* message) {
    String cmd = payload != nullptr ? String(payload) : String();
    cmd.trim();
    cmd.toUpperCase();
    if(cmd.length() == 0) {
        *message = "empty command";
        return false;
    }

    if(cmd == "START" || cmd == "BUTTON_START") {
        return runAction("start", message);
    }
    if(cmd == "STOP" || cmd == "BUTTON_STOP") {
        return runAction("stop", message);
    }
    if(cmd == "IDLE" || cmd == "MODE_IDLE" || cmd == "MODE_NORMAL") {
        return runAction("idle", message);
    }
    if(cmd == "IDLE_GBW" || cmd == "MODE_GBW") {
        return runAction("idle_gbw", message);
    }

    int32_t value = 0;
    if(parseIntArgument(cmd, "SET_RPM", &value) || parseIntArgument(cmd, "RPM", &value)) {
        return diag_api_set_rpm(value, message);
    }
    if(parseIntArgument(cmd, "STEP_RPM", &value)) {
        return diag_api_set_rpm(static_cast<int32_t>(setRPM) + value, message);
    }
    if(parseIntArgument(cmd, "SET_WEIGHT", &value) || parseIntArgument(cmd, "WEIGHT", &value)) {
        return diag_api_set_weight(value, message);
    }
    if(parseIntArgument(cmd, "STEP_WEIGHT", &value)) {
        return diag_api_set_weight(static_cast<int32_t>(setWeight) + value, message);
    }

    *message = "unknown esp-now command";
    return false;
}
}

bool espnow_control_start() {
    if(espnowActive) {
        return true;
    }

    if(espnowQueue == nullptr) {
        espnowQueue = xQueueCreate(ESPNOW_RX_QUEUE_LEN, sizeof(EspNowEvent));
        if(espnowQueue == nullptr) {
            return false;
        }
    } else {
        xQueueReset(espnowQueue);
    }

    WiFi.mode(WIFI_STA);
    // BLE is active on this firmware for the scale/console task, so keep
    // WiFi modem sleep enabled instead of tripping the BT coexistence abort.
    WiFi.setSleep(true);

    espnowChannelValue = default_espnow_channel;
    if(esp_wifi_set_channel(espnowChannelValue, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }

    if(esp_now_init() != ESP_OK) {
        return false;
    }

    if(esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
        esp_now_deinit();
        return false;
    }

    lastRxAt = 0;
    rxCount = 0;
    lastCommand[0] = '\0';
    espnowActive = true;
    Serial.printf("[espnow] started channel=%u modem_sleep=on\n", static_cast<unsigned>(espnowChannelValue));
    return true;
}

void espnow_control_stop() {
    if(!espnowActive) {
        return;
    }

    esp_now_unregister_recv_cb();
    esp_now_deinit();
    if(espnowQueue != nullptr) {
        xQueueReset(espnowQueue);
    }
    WiFi.mode(WIFI_MODE_NULL);
    espnowActive = false;
    Serial.println("[espnow] stopped");
}

void espnow_control_poll() {
    if(!espnowActive || espnowQueue == nullptr) {
        return;
    }

    EspNowEvent event = {};
    while(xQueueReceive(espnowQueue, &event, 0) == pdTRUE) {
        lastRxAt = millis();
        ++rxCount;
        strncpy(lastCommand, event.payload, sizeof(lastCommand) - 1);
        lastCommand[sizeof(lastCommand) - 1] = '\0';

        String message;
        const bool ok = handleEspNowCommand(event.payload, &message);
        Serial.printf("[espnow] cmd=\"%s\" ok=%s msg=%s\n",
                      event.payload,
                      ok ? "true" : "false",
                      message.c_str());
        disp_updateRequired = true;
    }
}

bool espnow_control_is_active() {
    return espnowActive;
}

uint8_t espnow_control_channel() {
    return espnowChannelValue;
}

String espnow_control_status_text() {
    if(!espnowActive) {
        return "ESP-NOW off";
    }

    String out = "ESP-NOW ch";
    out += String(static_cast<unsigned>(espnowChannelValue));
    if(lastRxAt > 0) {
        out += " rx ";
        out += String(static_cast<unsigned long>(rxCount));
    }
    return out;
}
