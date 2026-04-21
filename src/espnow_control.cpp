#include "espnow_control.h"

#include <cstring>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "definitions.h"
#include "comm.h"
#include "diag_console.h"
#include "gbw.h"
#include "display.h"
#include "main.h"
#include "motor_hal.h"
#include "wifi_ui.h"
#include <sg98_remote_protocol.h>

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

const char* stateTextCompact(States value) {
    switch(value) {
        case SLEEPING: return "SLEEPING";
        case IDLE: return "IDLE";
        case IDLE_GBW: return "IDLE_GBW";
        case GRINDING: return "GRINDING";
        case PURGING: return "PURGING";
        case GRINDING_GBW: return "GRINDING_GBW";
        case MENU1: return "MENU1";
        case MENU2: return "MENU2";
        case MENU3: return "MENU3";
        case MENU4: return "MENU4";
        case CALIBRATING: return "CALIBRATING";
        case MOTOR_SELECT: return "MOTOR_SELECT";
        default: return "UNKNOWN";
    }
}

const char* commTextCompact(COMM_STATUS value) {
    switch(value) {
        case COMM_DISCONNECTED: return "DISCONNECTED";
        case COMM_CONNECTED: return "CONNECTED";
        default: return "UNKNOWN";
    }
}

const char* motorTextCompact(SYSTEM_STATUS value) {
    switch(value) {
        case MOTOR_NOT_CONNECTED: return "NOT_CONNECTED";
        case MOTOR_NOT_READY: return "NOT_READY";
        case MOTOR_READY: return "READY";
        case MOTOR_ENABLED: return "ENABLED";
        case MOTOR_FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

const char* scaleTextCompact(SCALE_STATUS value) {
    switch(value) {
        case SCALE_DISCONNECTED: return "DISCONNECTED";
        case SCALE_CONNECTED: return "CONNECTED";
        case SCALE_CONNECTING: return "CONNECTING";
        case SCALE_LOST: return "LOST";
        default: return "UNKNOWN";
    }
}

const menuEntry* menuByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        case 4: return Menu4;
        default: return nullptr;
    }
}

uint8_t menuCountByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return NUM_MENU1_ITEMS;
        case 2: return NUM_MENU2_ITEMS;
        case 3: return NUM_MENU3_ITEMS;
        case 4: return NUM_MENU4_ITEMS;
        default: return 0;
    }
}

uint8_t currentMenuNumber() {
    switch(state) {
        case MENU1: return 1;
        case MENU2: return 2;
        case MENU3: return 3;
        case MENU4: return 4;
        default: return 0;
    }
}

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

bool parseSetMenuCommand(const String& raw, uint8_t* menuNum, uint8_t* itemIdx, int32_t* value) {
    if(!raw.startsWith("SET_MENU=")) {
        return false;
    }

    const String body = raw.substring(strlen("SET_MENU="));
    const int firstComma = body.indexOf(',');
    const int secondComma = body.indexOf(',', firstComma + 1);
    if(firstComma <= 0 || secondComma <= firstComma + 1) {
        return false;
    }

    *menuNum = static_cast<uint8_t>(body.substring(0, firstComma).toInt());
    *itemIdx = static_cast<uint8_t>(body.substring(firstComma + 1, secondComma).toInt());
    *value = body.substring(secondComma + 1).toInt();
    return true;
}

bool ensurePeer(const uint8_t* mac_addr) {
    if(mac_addr == nullptr) {
        return false;
    }

    if(esp_now_is_peer_exist(mac_addr)) {
        return true;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac_addr, sizeof(peer.peer_addr));
    peer.channel = espnowChannelValue;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

bool sendFrame(const uint8_t* mac_addr, const String& frame) {
    if(mac_addr == nullptr || frame.length() == 0) {
        return false;
    }
    if(frame.length() > 240) {
        Serial.printf("[espnow] skip oversized frame len=%u\n", static_cast<unsigned>(frame.length()));
        return false;
    }
    if(!ensurePeer(mac_addr)) {
        Serial.println("[espnow] peer setup failed");
        return false;
    }
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(frame.c_str());
    return esp_now_send(mac_addr, payload, frame.length() + 1) == ESP_OK;
}

String buildStatusFrame() {
    String out;
    out.reserve(192);
    out += sg98_protocol::kFrameStatus;
    out += sg98_protocol::kFrameSeparator;
    out += stateTextCompact(state);
    out += sg98_protocol::kFrameSeparator;
    out += motor_get_type_label();
    out += sg98_protocol::kFrameSeparator;
    out += commTextCompact(commStatus);
    out += sg98_protocol::kFrameSeparator;
    out += motorTextCompact(currentStatus);
    out += sg98_protocol::kFrameSeparator;
    out += scaleTextCompact(scaleStatus);
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<int>(error));
    out += sg98_protocol::kFrameSeparator;
    out += String(setRPM);
    out += sg98_protocol::kFrameSeparator;
    out += String(motor_setRPM);
    out += sg98_protocol::kFrameSeparator;
    out += String(motor_currentRPM);
    out += sg98_protocol::kFrameSeparator;
    out += String(motor_get_torque_percent());
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<long>(setWeight));
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<long>(currentWeight));
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<int>(currentMenuNumber()));
    out += sg98_protocol::kFrameSeparator;
    out += enterMenu ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<unsigned long>(comm_get_jmc_baud()));
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<unsigned long>(motor_get_preferred_jmc_baud()));
    return out;
}

String buildMenuFrame(uint8_t menuNum) {
    String out;
    const menuEntry* menu = menuByNumber(menuNum);
    const uint8_t count = menuCountByNumber(menuNum);
    if(menu == nullptr || count == 0) {
        return out;
    }

    out.reserve(176);
    out += sg98_protocol::kFrameMenu;
    out += sg98_protocol::kFrameSeparator;
    out += String(menuNum);
    out += sg98_protocol::kFrameSeparator;
    for(uint8_t idx = 0; idx < count; ++idx) {
        if(idx > 0) {
            out += sg98_protocol::kListSeparator;
        }
        out += String(menu[idx].value);
    }
    return out;
}

bool runAction(const char* action, const char* arg0, String* message) {
    return diag_api_run_action(action, arg0, nullptr, message);
}

bool handleActionCommand(const String& rawAction, String* message) {
    String actionName = rawAction;
    String arg0;
    const int comma = rawAction.indexOf(',');
    if(comma >= 0) {
        actionName = rawAction.substring(0, comma);
        arg0 = rawAction.substring(comma + 1);
        arg0.trim();
    }

    actionName.trim();
    actionName.toLowerCase();
    if(arg0.length() > 0) {
        arg0.toLowerCase();
    }

    if(actionName == "reset_network") {
        wifi_ui_reset_network_settings();
        if(message != nullptr) *message = "ok";
        return true;
    }

    return runAction(actionName.c_str(), arg0.length() > 0 ? arg0.c_str() : nullptr, message);
}

bool handleEspNowCommand(const String& rawPayload, String* message, bool* sendStatus, int8_t* menuRefresh) {
    String cmd = rawPayload;
    cmd.trim();
    if(cmd.length() == 0) {
        *message = "empty command";
        return false;
    }

    String cmdUpper = cmd;
    cmdUpper.toUpperCase();

    if(cmdUpper == sg98_protocol::kCmdGetStatus) {
        *sendStatus = true;
        *message = "ok";
        return true;
    }

    if(cmdUpper.startsWith("GET_MENU=")) {
        const uint8_t menuNum = static_cast<uint8_t>(cmd.substring(strlen("GET_MENU=")).toInt());
        if(menuByNumber(menuNum) == nullptr) {
            *message = "invalid menu";
            return false;
        }
        *menuRefresh = static_cast<int8_t>(menuNum);
        *message = "ok";
        return true;
    }

    uint8_t menuNum = 0;
    uint8_t itemIdx = 0;
    int32_t value = 0;
    if(parseSetMenuCommand(cmd, &menuNum, &itemIdx, &value)) {
        const bool ok = diag_api_set_menu_value(menuNum, itemIdx, value, message);
        *sendStatus = true;
        if(ok) {
            *menuRefresh = static_cast<int8_t>(menuNum);
        }
        return ok;
    }

    if(cmdUpper.startsWith("ACTION=")) {
        const bool ok = handleActionCommand(cmd.substring(strlen("ACTION=")), message);
        *sendStatus = true;
        return ok;
    }

    if(cmdUpper == "START" || cmdUpper == "BUTTON_START") {
        *sendStatus = true;
        return runAction("start", nullptr, message);
    }
    if(cmdUpper == "STOP" || cmdUpper == "BUTTON_STOP") {
        *sendStatus = true;
        return runAction("stop", nullptr, message);
    }
    if(cmdUpper == "IDLE" || cmdUpper == "MODE_IDLE" || cmdUpper == "MODE_NORMAL") {
        *sendStatus = true;
        return runAction("idle", nullptr, message);
    }
    if(cmdUpper == "IDLE_GBW" || cmdUpper == "MODE_GBW") {
        *sendStatus = true;
        return runAction("idle_gbw", nullptr, message);
    }

    if(parseIntArgument(cmdUpper, "SET_RPM", &value) || parseIntArgument(cmdUpper, "RPM", &value)) {
        *sendStatus = true;
        return diag_api_set_rpm(value, message);
    }
    if(parseIntArgument(cmdUpper, "STEP_RPM", &value)) {
        *sendStatus = true;
        return diag_api_set_rpm(static_cast<int32_t>(setRPM) + value, message);
    }
    if(parseIntArgument(cmdUpper, "SET_WEIGHT", &value) || parseIntArgument(cmdUpper, "WEIGHT", &value)) {
        *sendStatus = true;
        return diag_api_set_weight(value, message);
    }
    if(parseIntArgument(cmdUpper, "STEP_WEIGHT", &value)) {
        *sendStatus = true;
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
        bool sendStatus = false;
        int8_t menuRefresh = 0;
        const bool ok = handleEspNowCommand(String(event.payload), &message, &sendStatus, &menuRefresh);
        Serial.printf("[espnow] cmd=\"%s\" ok=%s msg=%s\n",
                      event.payload,
                      ok ? "true" : "false",
                      message.c_str());
        if(sendStatus || menuRefresh != 0 || !ok) {
            sendFrame(event.mac, sg98_protocol::ack(ok, message));
        }
        if(sendStatus) {
            sendFrame(event.mac, buildStatusFrame());
        }
        if(menuRefresh != 0) {
            sendFrame(event.mac, buildMenuFrame(static_cast<uint8_t>(menuRefresh)));
        }
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
