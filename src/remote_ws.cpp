#include "remote_ws.h"

#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include "burr_gap.h"
#include "remote_link.h"
#include "wifi_ui.h"
#include <sg98_remote_protocol.h>

namespace {

constexpr uint16_t kRemoteWsPort = 81;
constexpr uint32_t kMdnsRetryMs = 3000;
constexpr uint32_t kMdnsMinFreeHeap = 10000;
constexpr uint32_t kGaugeStatusPushMinMs = 60;

WebSocketsServer wsServer(kRemoteWsPort);
bool wsActive = false;
bool mdnsActive = false;
String mdnsHostname;
uint32_t lastMdnsAttemptAt = 0;
uint32_t mdnsLowHeapSkips = 0;
uint32_t lastGaugeStatusPushSequence = 0;
uint32_t gaugeStatusPushCount = 0;
unsigned long lastGaugeStatusPushAt = 0;

String buildHostname() {
    const uint64_t chipId = ESP.getEfuseMac();
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06llX", static_cast<unsigned long long>(chipId & 0xFFFFFFULL));
    return String("sg98-") + suffix;
}

void stopMdns() {
    if(!mdnsActive) {
        return;
    }

    MDNS.end();
    mdnsActive = false;
    Serial.println("[remote_ws] mdns stopped");
}

void startMdnsIfNeeded() {
    if(!wsActive || mdnsActive || WiFi.status() != WL_CONNECTED) {
        return;
    }
    if(lastMdnsAttemptAt + kMdnsRetryMs > millis()) {
        return;
    }

    lastMdnsAttemptAt = millis();
    const uint32_t freeHeap = ESP.getFreeHeap();
    if(freeHeap < kMdnsMinFreeHeap) {
        if((mdnsLowHeapSkips++ % 20U) == 0U) {
            Serial.printf("[remote_ws] mdns deferred low_heap=%lu\n",
                          static_cast<unsigned long>(freeHeap));
        }
        return;
    }
    if(MDNS.begin(mdnsHostname.c_str())) {
        MDNS.addService("sg98", "tcp", kRemoteWsPort);
        MDNS.addServiceTxt("sg98", "tcp", "path", "/");
        MDNS.addServiceTxt("sg98", "tcp", "role", "controller");
        MDNS.addServiceTxt("sg98", "tcp", "transport", "websocket");
        mdnsActive = true;
        mdnsLowHeapSkips = 0;
        Serial.printf("[remote_ws] mdns=%s.local port=%u\n",
                      mdnsHostname.c_str(),
                      static_cast<unsigned>(kRemoteWsPort));
    } else {
        Serial.println("[remote_ws] mdns start failed");
    }
}

void handleTextFrame(uint8_t clientNum, const uint8_t* payload, size_t length) {
    String command;
    command.reserve(length);
    for(size_t i = 0; i < length; ++i) {
        command += static_cast<char>(payload[i]);
    }
    command.trim();

    if(command.equalsIgnoreCase(sg98_protocol::kCmdGetEnvironment)) {
        String frame = remote_link_build_environment_frame();
        wsServer.sendTXT(clientNum, frame);
        Serial.println("[remote_ws] env");
        return;
    }

    String message;
    bool sendStatus = false;
    int8_t menuRefresh = 0;
    const bool ok = remote_link_handle_command(command, &message, &sendStatus, &menuRefresh);

    if(sendStatus || menuRefresh != 0 || !ok) {
        String frame = sg98_protocol::ack(ok, message);
        wsServer.sendTXT(clientNum, frame);
    }
    if(sendStatus) {
        String frame = remote_link_build_status_frame();
        if(wsServer.sendTXT(clientNum, frame)) {
            lastGaugeStatusPushSequence = burr_gap_read_change_sequence();
            lastGaugeStatusPushAt = millis();
        }
    }
    if(menuRefresh != 0) {
        String frame = remote_link_build_menu_frame(static_cast<uint8_t>(menuRefresh));
        wsServer.sendTXT(clientNum, frame);
    }

    Serial.printf("[remote_ws] cmd=\"%s\" ok=%s msg=%s\n",
                  command.c_str(),
                  ok ? "true" : "false",
                  message.c_str());
}

void onWsEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            Serial.printf("[remote_ws] client=%u connected path=%.*s\n",
                          static_cast<unsigned>(clientNum),
                          static_cast<int>(length),
                          payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
            break;
        case WStype_DISCONNECTED:
            Serial.printf("[remote_ws] client=%u disconnected\n", static_cast<unsigned>(clientNum));
            break;
        case WStype_TEXT:
            handleTextFrame(clientNum, payload, length);
            break;
        case WStype_ERROR:
            Serial.printf("[remote_ws] client=%u error len=%u payload=%.*s\n",
                          static_cast<unsigned>(clientNum),
                          static_cast<unsigned>(length),
                          static_cast<int>(length),
                          payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
            break;
        default:
            Serial.printf("[remote_ws] client=%u event=%d len=%u\n",
                          static_cast<unsigned>(clientNum),
                          static_cast<int>(type),
                          static_cast<unsigned>(length));
            break;
    }
}

}  // namespace

bool remote_ws_start() {
    if(wsActive) {
        return true;
    }

    if(mdnsHostname.isEmpty()) {
        mdnsHostname = buildHostname();
    }

    wsServer.begin();
    wsServer.onEvent(onWsEvent);
    wsActive = true;
    lastMdnsAttemptAt = 0;
    gaugeStatusPushCount = 0;
    lastGaugeStatusPushSequence = 0;
    lastGaugeStatusPushAt = 0;
    Serial.printf("[remote_ws] server port=%u\n", static_cast<unsigned>(kRemoteWsPort));
    return true;
}

void remote_ws_stop() {
    if(!wsActive) {
        return;
    }

    wsServer.close();
    stopMdns();
    wsActive = false;
    Serial.println("[remote_ws] stopped");
}

void remote_ws_poll() {
    if(!wsActive) {
        return;
    }

    wsServer.loop();
    startMdnsIfNeeded();
    if(mdnsActive && WiFi.status() != WL_CONNECTED) {
        stopMdns();
    }
}

bool remote_ws_broadcast_status_if_due(uint32_t gaugeReadChangeSequence) {
    if(!wsActive ||
       gaugeReadChangeSequence == 0 ||
       gaugeReadChangeSequence == lastGaugeStatusPushSequence ||
       wsServer.connectedClients(false) <= 0) {
        return false;
    }

    const unsigned long now = millis();
    if(lastGaugeStatusPushAt != 0 && now - lastGaugeStatusPushAt < kGaugeStatusPushMinMs) {
        return false;
    }

    String frame = remote_link_build_status_frame();
    if(!wsServer.broadcastTXT(frame)) {
        return false;
    }

    lastGaugeStatusPushSequence = gaugeReadChangeSequence;
    lastGaugeStatusPushAt = now;
    ++gaugeStatusPushCount;
    return true;
}

bool remote_ws_is_active() {
    return wsActive;
}
