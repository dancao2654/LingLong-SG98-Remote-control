#include "espnow_control.h"

#include <cstring>
#include <strings.h>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "burr_gap.h"
#include "controller_board.h"
#include "controller_ota_transport.h"
#include "definitions.h"
#include "comm.h"
#include "display.h"
#include "main.h"
#include "remote_link.h"
#include <sg98_remote_protocol.h>

namespace {
constexpr size_t ESPNOW_COMMAND_LEN = 250;
constexpr uint8_t ESPNOW_RX_QUEUE_LEN = 16;
constexpr uint32_t ESPNOW_GAUGE_STATUS_PUSH_MIN_MS = 60;
constexpr uint8_t ESPNOW_BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct EspNowEvent {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint16_t length = 0;
    uint8_t payload[ESPNOW_COMMAND_LEN] = {};
};

struct LatestEspNowEvent {
    bool valid = false;
    uint8_t mac[ESP_NOW_ETH_ALEN] = {};
    char payload[64] = {};
};

QueueHandle_t espnowQueue = nullptr;
bool espnowActive = false;
bool espnowCoexistActive = false;
volatile bool espnowWifiClientActive = false;
uint8_t espnowChannelValue = default_espnow_channel;
wifi_interface_t espnowInterface = WIFI_IF_STA;
    unsigned long lastRxAt = 0;
    unsigned long lastCommandRxAt = 0;
    unsigned long lastRecoveryRequestAt = 0;
    uint32_t rxCount = 0;
volatile uint32_t ingressCount = 0;
volatile uint32_t statusIngressCount = 0;
uint32_t dropCount = 0;
volatile uint32_t txDeliveredCount = 0;
volatile uint32_t txFailedCount = 0;
uint32_t txEnqueueFailedCount = 0;
uint32_t gaugeStatusPushCount = 0;
char lastCommand[ESPNOW_COMMAND_LEN] = "";
bool lastStatusPeerValid = false;
uint8_t lastStatusPeerMac[ESP_NOW_ETH_ALEN] = {};
uint32_t lastGaugeStatusPushSequence = 0;
unsigned long lastGaugeStatusPushAt = 0;
LatestEspNowEvent pendingStatusEvent;
LatestEspNowEvent pendingTargetEvent;
LatestEspNowEvent pendingDiscoveryEvent;
EspNowEvent pendingOtaEvent = {};
bool pendingOtaEventValid = false;
portMUX_TYPE espnowIngressMux = portMUX_INITIALIZER_UNLOCKED;

bool sendFrame(const uint8_t* mac_addr, const String& frame);
bool sendBytes(const uint8_t* mac_addr, const uint8_t* data, size_t length);

uint8_t currentWifiPrimaryChannel() {
    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if(esp_wifi_get_channel(&primary, &secondary) != ESP_OK) {
        return 0;
    }
    return primary;
}

void refreshCoexistChannel() {
    if(!espnowActive || !espnowCoexistActive) {
        return;
    }

    const uint8_t primary = currentWifiPrimaryChannel();
    if(primary == 0 || primary == espnowChannelValue) {
        return;
    }

    espnowChannelValue = primary;
    Serial.printf("[espnow] channel follow wifi=%u\n", static_cast<unsigned>(espnowChannelValue));
}

bool isGetStatusCommand(const char* payload) {
    return payload != nullptr && strcmp(payload, sg98_protocol::kCmdGetStatus) == 0;
}

bool isGetEnvironmentCommand(const char* payload) {
    return payload != nullptr && strcasecmp(payload, sg98_protocol::kCmdGetEnvironment) == 0;
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

bool isRecoveryRequestCommand(const char* payload) {
    return payload != nullptr && strcmp(payload, "RECOVER_ESPNOW") == 0;
}

String controllerDiscoveryName() {
    String out = "SG98 ";
    out += controller_board_target_name();
    return out;
}

String espNowInterfaceMacAddress() {
    uint8_t mac[ESP_NOW_ETH_ALEN] = {};
    if(esp_wifi_get_mac(espnowInterface, mac) != ESP_OK) {
        return WiFi.macAddress();
    }

    char out[18];
    snprintf(out,
             sizeof(out),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    return String(out);
}

bool macAddressUsable(const uint8_t* mac_addr) {
    if(mac_addr == nullptr) {
        return false;
    }

    bool anySet = false;
    bool allBroadcast = true;
    for(uint8_t idx = 0; idx < ESP_NOW_ETH_ALEN; ++idx) {
        anySet = anySet || mac_addr[idx] != 0;
        allBroadcast = allBroadcast && mac_addr[idx] == 0xFF;
    }
    return anySet && !allBroadcast;
}

void rememberStatusPeer(const uint8_t* mac_addr) {
    if(!macAddressUsable(mac_addr)) {
        return;
    }

    memcpy(lastStatusPeerMac, mac_addr, sizeof(lastStatusPeerMac));
    lastStatusPeerValid = true;
}

void noteGaugeStatusDelivered() {
    lastGaugeStatusPushSequence = burr_gap_read_change_sequence();
    lastGaugeStatusPushAt = millis();
}

String controllerDiscoveryFrame() {
    refreshCoexistChannel();
    return sg98_protocol::peer(controllerDiscoveryName(),
                               espNowInterfaceMacAddress(),
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
        memset(event->payload, 0, sizeof(event->payload));
        memcpy(event->payload, slot->payload, sizeof(slot->payload));
        event->length = static_cast<uint16_t>(strnlen(slot->payload, sizeof(slot->payload)) + 1);
        slot->valid = false;
        hasEvent = true;
    }
    portEXIT_CRITICAL(&espnowIngressMux);
    return hasEvent;
}

bool takePendingOtaEvent(EspNowEvent* event) {
    if(event == nullptr) return false;
    portENTER_CRITICAL(&espnowIngressMux);
    const bool available = pendingOtaEventValid;
    if(available) {
        *event = pendingOtaEvent;
        pendingOtaEventValid = false;
    }
    portEXIT_CRITICAL(&espnowIngressMux);
    return available;
}

void processEvent(const EspNowEvent& event) {
    lastRxAt = millis();
    ++rxCount;
    rememberStatusPeer(event.mac);
    if(sg98_ota_transport::isFrame(event.payload, event.length)) {
        lastCommandRxAt = lastRxAt;
        sg98_ota_transport::AckFrame ack = {};
        if(controller_ota_transport_handle(event.payload, event.length, &ack)) {
            (void)sendBytes(event.mac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
        }
        return;
    }
    const char* command = reinterpret_cast<const char*>(event.payload);
    if(controller_ota_transport_active() &&
       !isGetStatusCommand(command) &&
       !isDiscoverCommand(command) &&
       !isGetEnvironmentCommand(command) &&
       strncmp(command, "GET_MENU=", 9) != 0) {
        sendFrame(event.mac, sg98_protocol::ack(false, "controller OTA running"));
        return;
    }
    const bool wifiCredentials = strncmp(command, "WIFI|", 5) == 0;
    strncpy(lastCommand, wifiCredentials ? "WIFI|[redacted]" : command, sizeof(lastCommand) - 1);
    lastCommand[sizeof(lastCommand) - 1] = '\0';

    if(isRecoveryRequestCommand(command)) {
        lastCommandRxAt = lastRxAt;
        lastRecoveryRequestAt = lastRxAt;
        sendFrame(event.mac, sg98_protocol::ack(true, "recovery requested"));
        Serial.println("[espnow] recovery requested");
        return;
    }

    if(isDiscoverCommand(command)) {
        sendFrame(event.mac, controllerDiscoveryFrame());
        return;
    }

    if(isGetEnvironmentCommand(command)) {
        sendFrame(event.mac, remote_link_build_environment_frame());
        return;
    }

    String message;
    bool sendStatus = false;
    int8_t menuRefresh = 0;
    const bool ok = remote_link_handle_command(String(command), &message, &sendStatus, &menuRefresh);
    if(ok) {
        lastCommandRxAt = lastRxAt;
    }
    Serial.printf("[espnow] cmd=\"%s\" ok=%s msg=%s\n",
                  wifiCredentials ? "WIFI|[redacted]" : command,
                  ok ? "true" : "false",
                  message.c_str());
    if(sendStatus || menuRefresh != 0 || !ok) {
        sendFrame(event.mac, sg98_protocol::ack(ok, message));
    }
    if(sendStatus) {
        if(sendFrame(event.mac, remote_link_build_status_frame())) {
            noteGaugeStatusDelivered();
        }
    }
    if(menuRefresh != 0) {
        sendFrame(event.mac, remote_link_build_menu_frame(static_cast<uint8_t>(menuRefresh)));
    }
    if(!isGetStatusCommand(command) || !controller_is_menu_state(state)) {
        disp_updateRequired = true;
    }
}

void onEspNowSent(const esp_now_send_info_t*, esp_now_send_status_t status) {
    if(status == ESP_NOW_SEND_SUCCESS) {
        ++txDeliveredCount;
    } else {
        ++txFailedCount;
    }
}

void onEspNowReceive(const esp_now_recv_info_t* recv_info, const uint8_t* data, int data_len) {
    if(espnowQueue == nullptr || data == nullptr || data_len <= 0) {
        return;
    }
    ++ingressCount;
    if(espnowWifiClientActive) {
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
    event.length = static_cast<uint16_t>(copyLen);

    if(sg98_ota_transport::isFrame(event.payload, event.length)) {
        portENTER_CRITICAL_ISR(&espnowIngressMux);
        pendingOtaEvent = event;
        pendingOtaEventValid = true;
        portEXIT_CRITICAL_ISR(&espnowIngressMux);
        return;
    }

    const char* command = reinterpret_cast<const char*>(event.payload);
    if(isGetStatusCommand(command)) {
        ++statusIngressCount;
        portENTER_CRITICAL_ISR(&espnowIngressMux);
        storeLatestEvent(&pendingStatusEvent, event);
        portEXIT_CRITICAL_ISR(&espnowIngressMux);
        return;
    }

    if(isDiscoverCommand(command)) {
        portENTER_CRITICAL_ISR(&espnowIngressMux);
        storeLatestEvent(&pendingDiscoveryEvent, event);
        portEXIT_CRITICAL_ISR(&espnowIngressMux);
        return;
    }

    if(isTargetUpdateCommand(command)) {
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

    auto addPeerOnInterface = [&](wifi_interface_t interface) -> esp_err_t {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac_addr, sizeof(peer.peer_addr));
        // Channel 0 tracks the current WiFi channel, which keeps ESP-NOW usable
        // when AP+STA coexistence moves the radio to a home-router channel.
        peer.channel = 0;
        peer.ifidx = interface;
        peer.encrypt = false;
        return esp_now_add_peer(&peer);
    };

    const wifi_interface_t preferred = espnowInterface;
    esp_err_t err = addPeerOnInterface(preferred);
    if(err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        return true;
    }

    const wifi_interface_t fallbacks[] = {WIFI_IF_STA, WIFI_IF_AP};
    for(wifi_interface_t fallback : fallbacks) {
        if(fallback == preferred) {
            continue;
        }
        err = addPeerOnInterface(fallback);
        if(err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
            espnowInterface = fallback;
            Serial.printf("[espnow] peer_if_fallback=%s\n",
                          fallback == WIFI_IF_AP ? "AP" : "STA");
            return true;
        }
    }

    Serial.printf("[espnow] peer_setup_err=0x%x if=%s\n",
                  static_cast<unsigned>(err),
                  preferred == WIFI_IF_AP ? "AP" : "STA");
    return false;
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
        static unsigned long lastPeerSetupLogAt = 0;
        if(millis() - lastPeerSetupLogAt > 5000UL) {
            lastPeerSetupLogAt = millis();
            Serial.println("[espnow] peer setup deferred");
        }
        return false;
    }
    return sendBytes(mac_addr, reinterpret_cast<const uint8_t*>(frame.c_str()), frame.length() + 1);
}

bool sendBytes(const uint8_t* mac_addr, const uint8_t* data, size_t length) {
    if(mac_addr == nullptr || data == nullptr || length == 0 || length > 250) return false;
    if(!ensurePeer(mac_addr)) return false;
    const esp_err_t err = esp_now_send(mac_addr, data, length);
    if(err != ESP_OK) ++txEnqueueFailedCount;
    return err == ESP_OK;
}
}

bool espnow_control_start(bool coexistWithWifi) {
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

    const wifi_mode_t wifiMode = WiFi.getMode();
    if(coexistWithWifi) {
        if(wifiMode == WIFI_MODE_NULL) {
            WiFi.mode(WIFI_STA);
        }
        espnowInterface = (wifiMode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
        WiFi.setSleep(false);
        espnowChannelValue = currentWifiPrimaryChannel();
        if(espnowChannelValue == 0) {
            espnowChannelValue = default_espnow_channel;
            if(esp_wifi_set_channel(espnowChannelValue, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
                return false;
            }
        }
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(true);
        espnowInterface = WIFI_IF_STA;
        espnowChannelValue = default_espnow_channel;
        if(esp_wifi_set_channel(espnowChannelValue, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            return false;
        }
    }

    if(esp_now_init() != ESP_OK) {
        return false;
    }

    if(esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
        esp_now_deinit();
        return false;
    }
    if(esp_now_register_send_cb(onEspNowSent) != ESP_OK) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        return false;
    }

    lastRxAt = 0;
    lastCommandRxAt = 0;
    lastRecoveryRequestAt = 0;
    rxCount = 0;
    ingressCount = 0;
    statusIngressCount = 0;
    pendingDiscoveryEvent.valid = false;
    pendingOtaEventValid = false;
    txDeliveredCount = 0;
    txFailedCount = 0;
    txEnqueueFailedCount = 0;
    gaugeStatusPushCount = 0;
    lastCommand[0] = '\0';
    lastStatusPeerValid = false;
    lastGaugeStatusPushSequence = 0;
    lastGaugeStatusPushAt = 0;
    espnowWifiClientActive = false;
    espnowActive = true;
    espnowCoexistActive = coexistWithWifi;
    Serial.printf("[espnow] started channel=%u modem_sleep=%s coexist=%s\n",
                  static_cast<unsigned>(espnowChannelValue),
                  coexistWithWifi ? "off" : "on",
                  coexistWithWifi ? "true" : "false");
    return true;
}

void espnow_control_stop() {
    if(!espnowActive) {
        return;
    }

    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    if(espnowQueue != nullptr) {
        xQueueReset(espnowQueue);
    }
    if(!espnowCoexistActive) {
        WiFi.mode(WIFI_MODE_NULL);
    }
    espnowActive = false;
    espnowCoexistActive = false;
    espnowWifiClientActive = false;
    Serial.println("[espnow] stopped");
}

void espnow_control_poll() {
    if(!espnowActive || espnowQueue == nullptr) {
        return;
    }

    refreshCoexistChannel();
    controller_ota_transport_service();
    if(espnowWifiClientActive) {
        xQueueReset(espnowQueue);
        return;
    }

    EspNowEvent event = {};
    if(takePendingOtaEvent(&event)) {
        processEvent(event);
    }
    const uint8_t queueBudget = controller_is_menu_state(state) ? 1 : ESPNOW_RX_QUEUE_LEN;
    uint8_t processed = 0;
    while(processed < queueBudget && xQueueReceive(espnowQueue, &event, 0) == pdTRUE) {
        processEvent(event);
        ++processed;
    }

    if(takeLatestEvent(&pendingTargetEvent, &event)) {
        processEvent(event);
    }

    if(takeLatestEvent(&pendingStatusEvent, &event)) {
        processEvent(event);
    }
    if(takeLatestEvent(&pendingDiscoveryEvent, &event)) {
        processEvent(event);
    }
}

void espnow_control_poll_probe_status() {
    if(!espnowActive || espnowWifiClientActive) return;
    EspNowEvent event = {};
    if(takePendingOtaEvent(&event)) {
        processEvent(event);
    }
    if(takeLatestEvent(&pendingDiscoveryEvent, &event)) {
        processEvent(event);
    }
    if(takeLatestEvent(&pendingStatusEvent, &event)) {
        processEvent(event);
    }
}

bool espnow_control_push_status_if_due(uint32_t gaugeReadChangeSequence) {
    if(!espnowActive ||
       espnowWifiClientActive ||
       !lastStatusPeerValid ||
       gaugeReadChangeSequence == 0 ||
       gaugeReadChangeSequence == lastGaugeStatusPushSequence) {
        return false;
    }

    const unsigned long now = millis();
    if(lastGaugeStatusPushAt != 0 &&
       now - lastGaugeStatusPushAt < ESPNOW_GAUGE_STATUS_PUSH_MIN_MS) {
        return false;
    }

    if(!sendFrame(lastStatusPeerMac, remote_link_build_status_frame())) {
        return false;
    }

    lastGaugeStatusPushSequence = gaugeReadChangeSequence;
    lastGaugeStatusPushAt = now;
    ++gaugeStatusPushCount;
    return true;
}

bool espnow_control_broadcast_wifi_config(const String& ssid, const String& password) {
    if(!espnowActive || ssid.length() == 0) {
        return false;
    }

    const String frame = sg98_protocol::wifiConfig(ssid, password);
    const bool ok = sendFrame(ESPNOW_BROADCAST_MAC, frame);
    Serial.printf("[espnow] wifi_config broadcast=%s ssid=%s\n",
                  ok ? "ok" : "fail",
                  ssid.c_str());
    return ok;
}

void espnow_control_set_wifi_client_active(bool active) {
    if(espnowWifiClientActive == active) {
        return;
    }
    espnowWifiClientActive = active;
    if(active && espnowQueue != nullptr) {
        xQueueReset(espnowQueue);
    }
    Serial.printf("[espnow] wifi_client_hold=%s\n", active ? "on" : "off");
}

bool espnow_control_is_active() {
    return espnowActive;
}

bool espnow_control_is_wifi_client_active() {
    return espnowWifiClientActive;
}

uint8_t espnow_control_channel() {
    return espnowChannelValue;
}

unsigned long espnow_control_last_rx_at() {
    return lastRxAt;
}

unsigned long espnow_control_last_command_rx_at() {
    return lastCommandRxAt;
}

unsigned long espnow_control_last_recovery_request_at() {
    return lastRecoveryRequestAt;
}

String espnow_control_status_text() {
    if(!espnowActive) {
        return "ESP-NOW off";
    }

    String out = "ESP-NOW ch";
    out += String(static_cast<unsigned>(espnowChannelValue));
    if(espnowWifiClientActive) {
        out += " hold";
    }
    if(lastRxAt > 0) {
        out += " rx ";
        out += String(static_cast<unsigned long>(rxCount));
    }
    out += " ingress ";
    out += String(static_cast<unsigned long>(ingressCount));
    out += '/';
    out += String(static_cast<unsigned long>(statusIngressCount));
    if(dropCount > 0) {
        out += " drop ";
        out += String(static_cast<unsigned long>(dropCount));
    }
    out += " tx ";
    out += String(static_cast<unsigned long>(txDeliveredCount));
    out += '/';
    out += String(static_cast<unsigned long>(txFailedCount));
    if(txEnqueueFailedCount > 0) {
        out += " enqueue_fail ";
        out += String(static_cast<unsigned long>(txEnqueueFailedCount));
    }
    if(gaugeStatusPushCount > 0) {
        out += " push ";
        out += String(static_cast<unsigned long>(gaugeStatusPushCount));
    }
    return out;
}
