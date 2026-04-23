#include "espnow_control.h"

#include <cstring>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "controller_board.h"
#include "definitions.h"
#include "comm.h"
#include "display.h"
#include "main.h"
#include "remote_link.h"
#include <sg98_remote_protocol.h>

namespace {
constexpr size_t ESPNOW_COMMAND_LEN = 64;
constexpr uint8_t ESPNOW_RX_QUEUE_LEN = 16;

struct EspNowEvent {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    char payload[ESPNOW_COMMAND_LEN];
};

struct LatestEspNowEvent {
    bool valid = false;
    uint8_t mac[ESP_NOW_ETH_ALEN] = {};
    char payload[ESPNOW_COMMAND_LEN] = {};
};

QueueHandle_t espnowQueue = nullptr;
bool espnowActive = false;
uint8_t espnowChannelValue = default_espnow_channel;
unsigned long lastRxAt = 0;
uint32_t rxCount = 0;
uint32_t dropCount = 0;
char lastCommand[ESPNOW_COMMAND_LEN] = "";
LatestEspNowEvent pendingStatusEvent;
LatestEspNowEvent pendingTargetEvent;
portMUX_TYPE espnowIngressMux = portMUX_INITIALIZER_UNLOCKED;

bool sendFrame(const uint8_t* mac_addr, const String& frame);

bool isGetStatusCommand(const char* payload) {
    return payload != nullptr && strcmp(payload, sg98_protocol::kCmdGetStatus) == 0;
}

bool isTargetUpdateCommand(const char* payload) {
    if(payload == nullptr) {
        return false;
    }

    return strncmp(payload, "SET_RPM=", strlen("SET_RPM=")) == 0 ||
           strncmp(payload, "RPM=", strlen("RPM=")) == 0 ||
           strncmp(payload, "SET_WEIGHT=", strlen("SET_WEIGHT=")) == 0 ||
           strncmp(payload, "WEIGHT=", strlen("WEIGHT=")) == 0;
}

bool isDiscoverCommand(const char* payload) {
    return payload != nullptr && strcmp(payload, sg98_protocol::kCmdDiscover) == 0;
}

String controllerDiscoveryName() {
    String out = "SG98 ";
    out += controller_board_target_name();
    return out;
}

String controllerDiscoveryFrame() {
    return sg98_protocol::peer(controllerDiscoveryName(),
                               WiFi.macAddress(),
                               controller_board_target_name(),
                               espnowChannelValue);
}

void storeLatestEvent(LatestEspNowEvent* slot, const EspNowEvent& event) {
    if(slot == nullptr) {
        return;
    }

    memcpy(slot->mac, event.mac, sizeof(slot->mac));
    memcpy(slot->payload, event.payload, sizeof(slot->payload));
    slot->payload[sizeof(slot->payload) - 1] = '\0';
    slot->valid = true;
}

bool takeLatestEvent(LatestEspNowEvent* slot, EspNowEvent* event) {
    if(slot == nullptr || event == nullptr) {
        return false;
    }

    bool hasEvent = false;
    portENTER_CRITICAL(&espnowIngressMux);
    if(slot->valid) {
        memcpy(event->mac, slot->mac, sizeof(event->mac));
        memcpy(event->payload, slot->payload, sizeof(event->payload));
        event->payload[sizeof(event->payload) - 1] = '\0';
        slot->valid = false;
        hasEvent = true;
    }
    portEXIT_CRITICAL(&espnowIngressMux);
    return hasEvent;
}

void processEvent(const EspNowEvent& event) {
    lastRxAt = millis();
    ++rxCount;
    strncpy(lastCommand, event.payload, sizeof(lastCommand) - 1);
    lastCommand[sizeof(lastCommand) - 1] = '\0';

    if(isDiscoverCommand(event.payload)) {
        sendFrame(event.mac, controllerDiscoveryFrame());
        return;
    }

    String message;
    bool sendStatus = false;
    int8_t menuRefresh = 0;
    const bool ok = remote_link_handle_command(String(event.payload), &message, &sendStatus, &menuRefresh);
    Serial.printf("[espnow] cmd=\"%s\" ok=%s msg=%s\n",
                  event.payload,
                  ok ? "true" : "false",
                  message.c_str());
    if(sendStatus || menuRefresh != 0 || !ok) {
        sendFrame(event.mac, sg98_protocol::ack(ok, message));
    }
    if(sendStatus) {
        sendFrame(event.mac, remote_link_build_status_frame());
    }
    if(menuRefresh != 0) {
        sendFrame(event.mac, remote_link_build_menu_frame(static_cast<uint8_t>(menuRefresh)));
    }
    disp_updateRequired = true;
}

void onEspNowReceive(const esp_now_recv_info_t* recv_info, const uint8_t* data, int data_len) {
    if(espnowQueue == nullptr || data == nullptr || data_len <= 0) {
        return;
    }

    EspNowEvent event = {};
    const uint8_t* mac_addr = recv_info != nullptr ? recv_info->src_addr : nullptr;
    if(mac_addr != nullptr) {
        memcpy(event.mac, mac_addr, sizeof(event.mac));
    }

    const int copyLen = data_len < static_cast<int>(sizeof(event.payload) - 1) ? data_len : static_cast<int>(sizeof(event.payload) - 1);
    memcpy(event.payload, data, copyLen);
    event.payload[copyLen] = '\0';

    if(isGetStatusCommand(event.payload)) {
        portENTER_CRITICAL_ISR(&espnowIngressMux);
        storeLatestEvent(&pendingStatusEvent, event);
        portEXIT_CRITICAL_ISR(&espnowIngressMux);
        return;
    }

    if(isTargetUpdateCommand(event.payload)) {
        portENTER_CRITICAL_ISR(&espnowIngressMux);
        storeLatestEvent(&pendingTargetEvent, event);
        portEXIT_CRITICAL_ISR(&espnowIngressMux);
        return;
    }

    if(xQueueSend(espnowQueue, &event, 0) != pdPASS) {
        portENTER_CRITICAL_ISR(&espnowIngressMux);
        ++dropCount;
        portEXIT_CRITICAL_ISR(&espnowIngressMux);
    }
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
        processEvent(event);
    }

    if(takeLatestEvent(&pendingTargetEvent, &event)) {
        processEvent(event);
    }

    if(takeLatestEvent(&pendingStatusEvent, &event)) {
        processEvent(event);
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
    if(dropCount > 0) {
        out += " drop ";
        out += String(static_cast<unsigned long>(dropCount));
    }
    return out;
}
