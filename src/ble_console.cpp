#include "ble_console.h"

#include <NimBLEDevice.h>

namespace {
constexpr char BLE_CONSOLE_DEVICE_NAME[] = "SG98 Console";
constexpr char BLE_CONSOLE_SERVICE_UUID[] = "6c660001-b5a3-4d3f-88f8-df8e8d1f9801";
constexpr char BLE_CONSOLE_TX_UUID[] = "6c660002-b5a3-4d3f-88f8-df8e8d1f9801";

constexpr size_t BLE_CONSOLE_CHUNK_LEN = 20;

NimBLEServer* bleConsoleServer = nullptr;
NimBLECharacteristic* bleConsoleTx = nullptr;
bool bleConsoleReady = false;
bool bleConsoleAdvertisingEnabled = true;
volatile bool bleConsoleSubscribed = false;
volatile bool bleConsoleDumpPending = false;
uint32_t bleConsoleDumpReadyAt = 0;

void applyAdvertisingState() {
    if(!bleConsoleReady) {
        return;
    }

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if(advertising == nullptr) {
        return;
    }

    if(bleConsoleAdvertisingEnabled) {
        if(!advertising->isAdvertising()) {
            advertising->start();
        }
    } else if(advertising->isAdvertising()) {
        advertising->stop();
    }
}

class BleConsoleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        // Dump happens only in idle, so favor a faster one-shot transfer.
        server->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
        bleConsoleSubscribed = false;
        bleConsoleDumpPending = false;
        bleConsoleDumpReadyAt = 0;
    }

    void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
        bleConsoleSubscribed = false;
        bleConsoleDumpPending = false;
        bleConsoleDumpReadyAt = 0;
        applyAdvertisingState();
    }
} bleConsoleServerCallbacks;

class BleConsoleTxCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* /*characteristic*/, NimBLEConnInfo& /*connInfo*/, uint16_t subValue) override {
        const bool subscribed = (subValue & 0x01U) != 0;
        bleConsoleSubscribed = subscribed;
        if(subscribed) {
            bleConsoleDumpPending = true;
            bleConsoleDumpReadyAt = millis() + 150;
        }
    }
} bleConsoleTxCallbacks;
}

bool ble_console_init() {
    if(bleConsoleReady) {
        return true;
    }

    bleConsoleServer = NimBLEDevice::createServer();
    if(bleConsoleServer == nullptr) {
        return false;
    }
    bleConsoleServer->setCallbacks(&bleConsoleServerCallbacks);

    NimBLEService* service = bleConsoleServer->createService(BLE_CONSOLE_SERVICE_UUID);
    if(service == nullptr) {
        return false;
    }

    bleConsoleTx = service->createCharacteristic(
        BLE_CONSOLE_TX_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        20
    );

    if(bleConsoleTx == nullptr) {
        return false;
    }

    bleConsoleTx->setCallbacks(&bleConsoleTxCallbacks);

    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setName(BLE_CONSOLE_DEVICE_NAME);
    advertising->addServiceUUID(BLE_CONSOLE_SERVICE_UUID);
    advertising->enableScanResponse(true);
    advertising->setMinInterval(800);
    advertising->setMaxInterval(1200);

    bleConsoleReady = true;
    applyAdvertisingState();
    return true;
}

bool ble_console_has_client() {
    if(!bleConsoleReady || bleConsoleServer == nullptr) {
        return false;
    }

    return bleConsoleServer->getConnectedCount() > 0;
}

void ble_console_send_line(const char* line) {
    if(line == nullptr || line[0] == '\0' || !bleConsoleSubscribed || !ble_console_has_client() ||
       bleConsoleTx == nullptr) {
        return;
    }

    std::string payload(line);
    if(payload.back() != '\n') {
        payload.push_back('\n');
    }

    size_t offset = 0;
    while(offset < payload.size()) {
        size_t chunkLen = payload.size() - offset;
        if(chunkLen > BLE_CONSOLE_CHUNK_LEN) {
            chunkLen = BLE_CONSOLE_CHUNK_LEN;
        }

        bleConsoleTx->notify(reinterpret_cast<const uint8_t*>(payload.data() + offset), chunkLen);
        delay(2);
        offset += chunkLen;
    }
}

void ble_console_set_advertising_enabled(bool enabled) {
    bleConsoleAdvertisingEnabled = enabled;
    applyAdvertisingState();
}

bool ble_console_advertising_enabled() {
    return bleConsoleAdvertisingEnabled;
}

bool ble_console_take_dump_request() {
    if(!bleConsoleDumpPending || !bleConsoleSubscribed || millis() < bleConsoleDumpReadyAt ||
       !ble_console_has_client()) {
        return false;
    }

    bleConsoleDumpPending = false;
    return true;
}

const char* ble_console_device_name() {
    return BLE_CONSOLE_DEVICE_NAME;
}

const char* ble_console_service_uuid() {
    return BLE_CONSOLE_SERVICE_UUID;
}

const char* ble_console_tx_uuid() {
    return BLE_CONSOLE_TX_UUID;
}

const char* ble_console_rx_uuid() {
    return "";
}
