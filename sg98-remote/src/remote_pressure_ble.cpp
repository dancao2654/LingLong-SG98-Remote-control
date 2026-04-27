#include "remote_pressure_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

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
NimBLECharacteristic* g_commandChar = nullptr;
NimBLECharacteristic* g_pressureChar = nullptr;
NimBLECharacteristic* g_powerChar = nullptr;
NimBLECharacteristic* g_batteryChar = nullptr;
bool g_ready = false;
bool g_connected = false;
bool g_streaming = false;
bool g_stopping = false;
bool g_configResponsePending = false;
uint8_t g_configResponseIndex = 0;
uint32_t g_lastConfigResponseAt = 0;
uint32_t g_lastNotifyAt = 0;
uint16_t g_lastPressureCentiBar = 0;
int8_t g_lastBatteryPercent = -1;
Preferences g_bookooPrefs;
bool g_bookooPrefsReady = false;
bool g_configDirty = false;
uint32_t g_configDirtyAt = 0;

constexpr uint32_t kConfigResponseSpacingMs = 45;
constexpr uint32_t kConfigPersistDelayMs = 1800;
constexpr uint8_t kBookooFrameHeader = 0x02;
constexpr uint8_t kBookooConfigResponseType = 0x1A;
constexpr uint8_t kBookooCommandType = 0x0C;
constexpr uint8_t kBookooFirmwareVersion = 0x67;  // BooKooN shows this as v1.0.3.
constexpr const char* kBookooPrefsNamespace = "bookoo";
constexpr const char* kPrefMaxBrewDeciMinutes = "maxDm";
constexpr const char* kPrefInitialPressureDeciBar = "initDb";
constexpr const char* kPrefStandbyDeciMinutes = "stdbyDm";
constexpr const char* kPrefAutoStopSeconds = "autoS";

struct BookooProfilerConfig {
    uint16_t maxBrewDeciMinutes = 300;       // 30.0 minutes.
    uint16_t initialPressureDeciBar = 8;     // 0.8 bar.
    uint16_t standbyDeciMinutes = 50;        // 5.0 minutes.
    uint8_t autoStopSeconds = 30;
};

BookooProfilerConfig g_bookooConfig;

uint16_t clampConfigU16(uint16_t value, uint16_t minValue, uint16_t maxValue) {
    if(value < minValue) {
        return minValue;
    }
    if(value > maxValue) {
        return maxValue;
    }
    return value;
}

uint8_t clampConfigU8(uint8_t value, uint8_t minValue, uint8_t maxValue) {
    if(value < minValue) {
        return minValue;
    }
    if(value > maxValue) {
        return maxValue;
    }
    return value;
}

void sanitizeBookooConfig() {
    g_bookooConfig.maxBrewDeciMinutes = clampConfigU16(g_bookooConfig.maxBrewDeciMinutes, 10, 600);
    g_bookooConfig.initialPressureDeciBar = clampConfigU16(g_bookooConfig.initialPressureDeciBar, 0, 10);
    g_bookooConfig.standbyDeciMinutes = clampConfigU16(g_bookooConfig.standbyDeciMinutes, 10, 600);
    g_bookooConfig.autoStopSeconds = clampConfigU8(g_bookooConfig.autoStopSeconds, 5, 180);
}

void beginBookooPrefs() {
    if(g_bookooPrefsReady) {
        return;
    }
    g_bookooPrefsReady = g_bookooPrefs.begin(kBookooPrefsNamespace, false);
    if(!g_bookooPrefsReady) {
        return;
    }

    g_bookooConfig.maxBrewDeciMinutes =
        g_bookooPrefs.getUShort(kPrefMaxBrewDeciMinutes, g_bookooConfig.maxBrewDeciMinutes);
    g_bookooConfig.initialPressureDeciBar =
        g_bookooPrefs.getUShort(kPrefInitialPressureDeciBar, g_bookooConfig.initialPressureDeciBar);
    g_bookooConfig.standbyDeciMinutes =
        g_bookooPrefs.getUShort(kPrefStandbyDeciMinutes, g_bookooConfig.standbyDeciMinutes);
    g_bookooConfig.autoStopSeconds = g_bookooPrefs.getUChar(kPrefAutoStopSeconds, g_bookooConfig.autoStopSeconds);
    sanitizeBookooConfig();
}

void persistBookooConfig() {
    if(!g_bookooPrefsReady) {
        beginBookooPrefs();
    }
    if(!g_bookooPrefsReady) {
        return;
    }

    sanitizeBookooConfig();
    g_bookooPrefs.putUShort(kPrefMaxBrewDeciMinutes, g_bookooConfig.maxBrewDeciMinutes);
    g_bookooPrefs.putUShort(kPrefInitialPressureDeciBar, g_bookooConfig.initialPressureDeciBar);
    g_bookooPrefs.putUShort(kPrefStandbyDeciMinutes, g_bookooConfig.standbyDeciMinutes);
    g_bookooPrefs.putUChar(kPrefAutoStopSeconds, g_bookooConfig.autoStopSeconds);
    g_configDirty = false;
}

void scheduleBookooConfigPersist() {
    sanitizeBookooConfig();
    g_configDirty = true;
    g_configDirtyAt = millis();
}

void serviceBookooConfigPersist(uint32_t nowMs) {
    if(g_configDirty && static_cast<uint32_t>(nowMs - g_configDirtyAt) >= kConfigPersistDelayMs) {
        persistBookooConfig();
    }
}

void updateBookooConfigU16(uint16_t& field, uint16_t value, uint16_t minValue, uint16_t maxValue) {
    value = clampConfigU16(value, minValue, maxValue);
    if(field == value) {
        return;
    }
    field = value;
    scheduleBookooConfigPersist();
}

void updateBookooConfigU8(uint8_t& field, uint8_t value, uint8_t minValue, uint8_t maxValue) {
    value = clampConfigU8(value, minValue, maxValue);
    if(field == value) {
        return;
    }
    field = value;
    scheduleBookooConfigPersist();
}

uint8_t xorChecksum(const uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for(size_t i = 0; i < len; ++i) {
        checksum ^= data[i];
    }
    return checksum;
}

void finalizeBookooPacket(uint8_t* packet, size_t len) {
    if(packet == nullptr || len == 0) {
        return;
    }
    packet[len - 1] = xorChecksum(packet, len - 1);
}

void putBigEndian16(uint8_t* packet, size_t highIndex, uint16_t value) {
    packet[highIndex] = static_cast<uint8_t>((value >> 8) & 0xFF);
    packet[highIndex + 1] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t readBigEndian16(const std::string& value, size_t highIndex) {
    if(value.size() <= highIndex + 1) {
        return 0;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(value[highIndex])) << 8) |
                                 static_cast<uint8_t>(value[highIndex + 1]));
}

void setInitialCommandReadback() {
    if(g_commandChar == nullptr) {
        return;
    }
    uint8_t packet[20] = {
        kBookooFrameHeader, kBookooConfigResponseType, 0x05,
        'S', 'G', '9', '8', 'R', 'E', 'M', 'O',
        'T', 'E', '0', '0', '0', '1', 0x00, 0x00, 0x00,
    };
    finalizeBookooPacket(packet, sizeof(packet));
    g_commandChar->setValue(packet, sizeof(packet));
}

void notifyBookooConfigResponse(uint8_t index) {
    if(g_commandChar == nullptr || !g_connected) {
        return;
    }

    uint8_t packet[20] = {};
    packet[0] = kBookooFrameHeader;
    packet[1] = kBookooConfigResponseType;

    if(index == 0) {
        packet[2] = 0x00;
        putBigEndian16(packet, 3, g_bookooConfig.maxBrewDeciMinutes);
        putBigEndian16(packet, 5, g_bookooConfig.initialPressureDeciBar);
        putBigEndian16(packet, 9, g_bookooConfig.standbyDeciMinutes);
        packet[11] = g_bookooConfig.autoStopSeconds;
        packet[12] = static_cast<uint8_t>(min<uint16_t>(g_bookooConfig.initialPressureDeciBar, 0xFF));
        packet[13] = '0';
        packet[14] = '0';
        packet[15] = '0';
        packet[16] = '1';
    } else if(index == 1) {
        packet[2] = 0x03;
        packet[4] = kBookooFirmwareVersion;
    } else {
        packet[2] = 0x05;
        packet[3] = 'S';
        packet[4] = 'G';
        packet[5] = '9';
        packet[6] = '8';
        packet[7] = 'R';
        packet[8] = 'E';
        packet[9] = 'M';
        packet[10] = 'O';
        packet[11] = 'T';
        packet[12] = 'E';
        packet[13] = '0';
        packet[14] = '0';
        packet[15] = '0';
        packet[16] = '1';
    }

    finalizeBookooPacket(packet, sizeof(packet));
    g_commandChar->setValue(packet, sizeof(packet));
    g_commandChar->notify();
}

class PressureServerCallbacks : public NimBLEServerCallbacks {
  public:
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        g_connected = true;
        if(server != nullptr) {
            server->updateConnParams(connInfo.getConnHandle(), 8, 16, 0, 180);
        }
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        if(g_configDirty) {
            persistBookooConfig();
        }
        g_connected = false;
        g_streaming = false;
        if(!g_stopping && g_ready) {
            NimBLEDevice::startAdvertising();
        }
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
           static_cast<uint8_t>(value[1]) == kBookooCommandType) {
            const uint8_t command = static_cast<uint8_t>(value[2]);
            if(command == 0x00) {
                g_streaming = false;
            } else if(command == 0x01) {
                g_streaming = true;
            } else if(command == 0x02) {
                updateBookooConfigU16(g_bookooConfig.maxBrewDeciMinutes, readBigEndian16(value, 3), 10, 600);
            } else if(command == 0x03) {
                updateBookooConfigU16(g_bookooConfig.initialPressureDeciBar, readBigEndian16(value, 3), 0, 10);
            } else if(command == 0x06) {
                g_configResponsePending = true;
                g_configResponseIndex = 0;
                g_lastConfigResponseAt = 0;
            } else if(command == 0x07) {
                updateBookooConfigU16(g_bookooConfig.standbyDeciMinutes, readBigEndian16(value, 3), 10, 600);
            } else if(command == 0x10 && value.size() >= 4) {
                updateBookooConfigU8(g_bookooConfig.autoStopSeconds, static_cast<uint8_t>(value[3]), 5, 180);
            }
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

void setPowerCharacteristic(uint8_t percent, bool notify) {
    if(g_powerChar == nullptr) {
        return;
    }
    uint8_t packet[10] = {kBookooFrameHeader, 0x1D, 0x00, 0x00, percent, 0x00, 0x00, 0x00, 0x00, 0x00};
    finalizeBookooPacket(packet, sizeof(packet));
    g_powerChar->setValue(packet, sizeof(packet));
    if(notify && g_connected) {
        g_powerChar->notify();
    }
}

void updateBatteryCharacteristics(int8_t percent) {
    if(percent == g_lastBatteryPercent) {
        return;
    }
    g_lastBatteryPercent = percent;
    const uint8_t value = batteryByte(percent);
    setPowerCharacteristic(value, true);
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

    g_stopping = false;
    beginBookooPrefs();
    NimBLEDevice::init(kBookooDeviceName);
    NimBLEDevice::setMTU(23);

    g_server = NimBLEDevice::createServer();
    if(g_server == nullptr) {
        return;
    }
    g_server->setCallbacks(&g_serverCallbacks, false);

    NimBLEService* pressureService = g_server->createService(kBookooPressureServiceUuid);
    if(pressureService == nullptr) {
        return;
    }
    g_commandChar =
        pressureService->createCharacteristic(kBookooCommandCharUuid,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
                                                  NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,
                                              20);
    if(g_commandChar != nullptr) {
        g_commandChar->setCallbacks(&g_commandCallbacks);
        setInitialCommandReadback();
    }

    g_pressureChar = pressureService->createCharacteristic(kBookooPressureCharUuid,
                                                           NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                           10);
    g_powerChar = pressureService->createCharacteristic(kBookooPowerCharUuid,
                                                        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                        10);
    setPowerCharacteristic(0, false);
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

void remote_pressure_ble_end() {
    if(!g_ready && !NimBLEDevice::isInitialized()) {
        return;
    }

    g_stopping = true;
    if(g_configDirty) {
        persistBookooConfig();
    }
    if(NimBLEDevice::isInitialized()) {
        NimBLEDevice::stopAdvertising();
        if(g_server != nullptr) {
            for(uint16_t connHandle : g_server->getPeerDevices()) {
                g_server->disconnect(connHandle);
            }
        }
        NimBLEDevice::deinit(true);
    }

    g_server = nullptr;
    g_commandChar = nullptr;
    g_pressureChar = nullptr;
    g_powerChar = nullptr;
    g_batteryChar = nullptr;
    g_ready = false;
    g_connected = false;
    g_streaming = false;
    g_configResponsePending = false;
    g_configResponseIndex = 0;
    g_lastConfigResponseAt = 0;
    g_lastNotifyAt = 0;
    g_lastPressureCentiBar = 0;
    g_lastBatteryPercent = -1;
    g_stopping = false;
}

void remote_pressure_ble_service(uint16_t pressureCentiBar, int8_t batteryPercent, uint32_t nowMs) {
    if(!g_ready || g_pressureChar == nullptr) {
        return;
    }

    updateBatteryCharacteristics(batteryPercent);
    serviceBookooConfigPersist(nowMs);
    g_lastPressureCentiBar = pressureCentiBar;
    if(g_configResponsePending && g_commandChar != nullptr && g_connected &&
       (g_lastConfigResponseAt == 0 || (nowMs - g_lastConfigResponseAt) >= kConfigResponseSpacingMs)) {
        notifyBookooConfigResponse(g_configResponseIndex);
        g_lastConfigResponseAt = nowMs;
        ++g_configResponseIndex;
        if(g_configResponseIndex >= 3) {
            g_configResponsePending = false;
        }
    }
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
