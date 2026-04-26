#include "remote_pressure_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "remote_config.h"

namespace {

constexpr const char* kBookooDeviceName = "BOOKOO_EM SG98";
constexpr const char* kBookooPressureServiceUuid = "0FFF";
constexpr const char* kBookooCommandCharUuid = "FF01";
constexpr const char* kBookooPressureCharUuid = "FF02";
constexpr const char* kBookooPowerCharUuid = "FF03";
constexpr const char* kBatteryServiceUuid = "180F";
constexpr const char* kBatteryLevelCharUuid = "2A19";
constexpr const char* kDeviceInfoServiceUuid = "180A";
constexpr const char* kModelNumberCharUuid = "2A24";
constexpr const char* kFirmwareRevisionCharUuid = "2A26";
constexpr const char* kManufacturerNameCharUuid = "2A29";
constexpr uint16_t kFastAdvertiseInterval = 80;  // 50 ms, in 0.625 ms BLE units.

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_pressureChar = nullptr;
NimBLECharacteristic* g_powerChar = nullptr;
NimBLECharacteristic* g_batteryChar = nullptr;
bool g_ready = false;
bool g_connected = false;
bool g_streaming = false;
uint32_t g_lastNotifyAt = 0;
uint16_t g_lastPressureCentiBar = 0;
int8_t g_lastBatteryPercent = -1;

class PressureServerCallbacks : public NimBLEServerCallbacks {
  public:
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        g_connected = true;
        if(server != nullptr) {
            server->updateConnParams(connInfo.getConnHandle(), 8, 16, 0, 180);
        }
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        g_connected = false;
        g_streaming = false;
        NimBLEDevice::startAdvertising();
    }
};

class PressureCommandCallbacks : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        if(characteristic == nullptr) {
            return;
        }
        const std::string value = characteristic->getValue();
        if(value.size() >= 3 && static_cast<uint8_t>(value[0]) == 0x02 &&
           static_cast<uint8_t>(value[1]) == 0x0C) {
            g_streaming = static_cast<uint8_t>(value[2]) != 0;
        } else if(!value.empty()) {
            g_streaming = true;
        }
    }
};

PressureServerCallbacks g_serverCallbacks;
PressureCommandCallbacks g_commandCallbacks;

uint8_t batteryByte(int8_t percent) {
    if(percent < 0) {
        return 0;
    }
    if(percent > 100) {
        return 100;
    }
    return static_cast<uint8_t>(percent);
}

void updateBatteryCharacteristics(int8_t percent) {
    if(percent == g_lastBatteryPercent) {
        return;
    }
    g_lastBatteryPercent = percent;
    const uint8_t value = batteryByte(percent);
    if(g_powerChar != nullptr) {
        g_powerChar->setValue(&value, sizeof(value));
        if(g_connected) {
            g_powerChar->notify();
        }
    }
    if(g_batteryChar != nullptr) {
        g_batteryChar->setValue(&value, sizeof(value));
        if(g_connected) {
            g_batteryChar->notify();
        }
    }
}

}  // namespace

void remote_pressure_ble_begin() {
    if(g_ready) {
        return;
    }

    NimBLEDevice::init(kBookooDeviceName);
    NimBLEDevice::setMTU(23);

    g_server = NimBLEDevice::createServer();
    if(g_server == nullptr) {
        return;
    }
    g_server->setCallbacks(&g_serverCallbacks);

    NimBLEService* pressureService = g_server->createService(kBookooPressureServiceUuid);
    if(pressureService == nullptr) {
        return;
    }
    NimBLECharacteristic* commandChar =
        pressureService->createCharacteristic(kBookooCommandCharUuid,
                                              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
                                              8);
    if(commandChar != nullptr) {
        commandChar->setCallbacks(&g_commandCallbacks);
    }

    g_pressureChar = pressureService->createCharacteristic(kBookooPressureCharUuid,
                                                           NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                           10);
    g_powerChar = pressureService->createCharacteristic(kBookooPowerCharUuid,
                                                        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                        1);
    pressureService->start();

    NimBLEService* batteryService = g_server->createService(kBatteryServiceUuid);
    if(batteryService != nullptr) {
        g_batteryChar = batteryService->createCharacteristic(kBatteryLevelCharUuid,
                                                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                             1);
        const uint8_t initialBattery = 0;
        g_batteryChar->setValue(&initialBattery, sizeof(initialBattery));
        batteryService->start();
    }

    NimBLEService* deviceInfoService = g_server->createService(kDeviceInfoServiceUuid);
    if(deviceInfoService != nullptr) {
        NimBLECharacteristic* modelChar =
            deviceInfoService->createCharacteristic(kModelNumberCharUuid, NIMBLE_PROPERTY::READ);
        if(modelChar != nullptr) {
            modelChar->setValue("BOOKOO_EM");
        }
        NimBLECharacteristic* firmwareChar =
            deviceInfoService->createCharacteristic(kFirmwareRevisionCharUuid, NIMBLE_PROPERTY::READ);
        if(firmwareChar != nullptr) {
            firmwareChar->setValue("SG98");
        }
        NimBLECharacteristic* manufacturerChar =
            deviceInfoService->createCharacteristic(kManufacturerNameCharUuid, NIMBLE_PROPERTY::READ);
        if(manufacturerChar != nullptr) {
            manufacturerChar->setValue("LingLong");
        }
        deviceInfoService->start();
    }

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisingInterval(kFastAdvertiseInterval);
    advertising->enableScanResponse(false);
    advertising->setName(kBookooDeviceName);
    advertising->addServiceUUID(kBookooPressureServiceUuid);
    advertising->addServiceUUID(kBatteryServiceUuid);
    advertising->addServiceUUID(kDeviceInfoServiceUuid);
    advertising->start();

    g_ready = true;
}

void remote_pressure_ble_service(uint16_t pressureCentiBar, int8_t batteryPercent, uint32_t nowMs) {
    if(!g_ready || g_pressureChar == nullptr) {
        return;
    }

    updateBatteryCharacteristics(batteryPercent);
    g_lastPressureCentiBar = pressureCentiBar;
    if(!g_connected || !g_streaming || (nowMs - g_lastNotifyAt) < remote_config::kPressureProfilerBleNotifyMs) {
        return;
    }

    uint8_t packet[10] = {};
    packet[0] = 0x03;
    packet[1] = 0x0B;
    packet[4] = static_cast<uint8_t>((pressureCentiBar >> 8) & 0xFF);
    packet[5] = static_cast<uint8_t>(pressureCentiBar & 0xFF);
    packet[9] = static_cast<uint8_t>((packet[0] + packet[1] + packet[4] + packet[5]) & 0xFF);

    g_pressureChar->setValue(packet, sizeof(packet));
    g_pressureChar->notify();
    g_lastNotifyAt = nowMs;
}

bool remote_pressure_ble_ready() {
    return g_ready;
}

bool remote_pressure_ble_connected() {
    return g_connected;
}

bool remote_pressure_ble_streaming() {
    return g_streaming;
}
