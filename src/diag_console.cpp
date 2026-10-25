#include "diag_console.h"

#include <cstdlib>
#include <cstring>

#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "burr_gap.h"
#include "controller_board.h"
#include "definitions.h"
#include "display.h"
#include "error_history.h"
#include "gbw.h"
#include "main.h"
#include "ble_console.h"
#include "comm.h"
#include "controller_env.h"
#include "controller_ota.h"
#include "espnow_control.h"
#include "motor_hal.h"
#include "pdata.h"
#include "radio_mode.h"
#include "remote_ws.h"
#include "sg98_ota_download.h"
#include "sg98_ota_source.h"
#include "sg98_version.h"
#include "wifi_ui.h"

namespace {
bool streamEnabled = false;
unsigned long lastStreamAt = 0;
bool pendingApiReboot = false;
unsigned long pendingApiRebootAt = 0;
uint8_t lastObservedError = 0xFF;
uint16_t lastObservedFaultCode = 0xFFFF;
uint8_t lastObservedCommStatus = 0xFF;
uint8_t lastObservedMotorStatus = 0xFF;
uint8_t lastObservedScaleStatus = 0xFF;
char serialLineBuffer[384];
size_t serialLineLen = 0;
char otaMessageBuffer[160];

constexpr unsigned long kApiRebootDelayMs = 750;
constexpr uint8_t kGaugeProbeFrameBits = 24;
constexpr uint32_t kGaugeProbeFrameGapUs = 5000;
constexpr uint16_t kGaugeProbeDefaultMs = 1000;
constexpr uint16_t kGaugeProbeMinMs = 50;
constexpr uint16_t kGaugeProbeMaxMs = 5000;
constexpr int32_t kGaugeProbeMaxReasonableGapUm = 60000;

struct OtaWifiGuard {
    bool wifiWasActive = false;
    bool espNowWasActive = false;
};

struct GaugeProbeResult {
    uint32_t samples = 0;
    uint32_t clkTransitions = 0;
    uint32_t clkRising = 0;
    uint32_t clkFalling = 0;
    uint32_t dataTransitions = 0;
    uint32_t frames = 0;
    uint32_t validFrames = 0;
    uint32_t lastFrame = 0;
    int32_t lastUm = 0;
    uint8_t initialClk = 0;
    uint8_t initialData = 0;
    uint8_t finalClk = 0;
    uint8_t finalData = 0;
};

const char* stateText(States value) {
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
        case MENU5: return "MENU5";
        case CALIBRATING: return "CALIBRATING";
        case MOTOR_SELECT: return "MOTOR_SELECT";
        case NETWORK_INFO: return "NETWORK_INFO";
        case ABOUT_INFO: return "ABOUT_INFO";
    }
    return "UNKNOWN";
}

const char* commText(COMM_STATUS value) {
    switch(value) {
        case COMM_DISCONNECTED: return "DISCONNECTED";
        case COMM_CONNECTED: return "CONNECTED";
        case COMM_INVALID_STATUS: return "INVALID";
    }
    return "UNKNOWN";
}

const char* shortStateText(States value) {
    switch(value) {
        case SLEEPING: return "SLEEP";
        case IDLE: return "IDLE";
        case IDLE_GBW: return "IDLE_GBW";
        case GRINDING: return "GRIND";
        case PURGING: return "PURGE";
        case GRINDING_GBW: return "GRD_GBW";
        case MENU1: return "MENU1";
        case MENU2: return "MENU2";
        case MENU3: return "MENU3";
        case MENU4: return "MENU4";
        case MENU5: return "MENU5";
        case CALIBRATING: return "CAL";
        case MOTOR_SELECT: return "DRVSEL";
        case NETWORK_INFO: return "NET";
        case ABOUT_INFO: return "ABOUT";
    }
    return "UNK";
}

const char* motorText(SYSTEM_STATUS value) {
    switch(value) {
        case MOTOR_NOT_CONNECTED: return "NOT_CONNECTED";
        case MOTOR_NOT_READY: return "NOT_READY";
        case MOTOR_READY: return "READY";
        case MOTOR_ENABLED: return "ENABLED";
        case MOTOR_FAULT: return "FAULT";
        case MOTOR_INVALID: return "INVALID";
    }
    return "UNKNOWN";
}

const char* scaleText(SCALE_STATUS value) {
    switch(value) {
        case SCALE_DISCONNECTED: return "DISCONNECTED";
        case SCALE_CONNECTED: return "CONNECTED";
        case SCALE_CONNECTING: return "CONNECTING";
        case SCALE_LOST: return "LOST";
        case INVALID_SCALE_STATUS: return "INVALID";
    }
    return "UNKNOWN";
}

const char* shortCommText(COMM_STATUS value) {
    switch(value) {
        case COMM_DISCONNECTED: return "DISC";
        case COMM_CONNECTED: return "CONN";
        case COMM_INVALID_STATUS: return "INV";
    }
    return "UNK";
}

const char* shortMotorText(SYSTEM_STATUS value) {
    switch(value) {
        case MOTOR_NOT_CONNECTED: return "NC";
        case MOTOR_NOT_READY: return "NR";
        case MOTOR_READY: return "RDY";
        case MOTOR_ENABLED: return "EN";
        case MOTOR_FAULT: return "FLT";
        case MOTOR_INVALID: return "INV";
    }
    return "UNK";
}

const char* shortScaleText(SCALE_STATUS value) {
    switch(value) {
        case SCALE_DISCONNECTED: return "DISC";
        case SCALE_CONNECTED: return "CONN";
        case SCALE_CONNECTING: return "CONG";
        case SCALE_LOST: return "LOST";
        case INVALID_SCALE_STATUS: return "INV";
    }
    return "UNK";
}

const char* shortScaleTextRaw(uint8_t value) {
    return shortScaleText(static_cast<SCALE_STATUS>(value));
}

const char* shortCommTextRaw(uint8_t value) {
    return shortCommText(static_cast<COMM_STATUS>(value));
}

const char* shortMotorTextRaw(uint8_t value) {
    return shortMotorText(static_cast<SYSTEM_STATUS>(value));
}

const char* shortStateTextRaw(uint8_t value) {
    return shortStateText(static_cast<States>(value));
}

const char* profileText(CommProfile profile) {
    switch(profile) {
        case COMM_PROFILE_JMC: return "JMC";
        case COMM_PROFILE_RT: return "RT";
        case COMM_PROFILE_KRS: return "KRS";
        case COMM_PROFILE_SEW_MOVILINK: return "SEW";
        case COMM_PROFILE_TSE: return "TSE";
    }
    return "UNK";
}

const char* errorText(uint8_t code) {
    switch(code) {
        case 0: return "clear";
        case 1: return "drive_conn";
        case 2: return "drive_lost";
        case 3: return "drv_fault";
        case 4: return "pkt_loss";
        case 5: return "mutex";
        case 6: return "stalled";
        case 7: return "write_fail";
        case 8: return "first_boot";
        case 100: return "set_rpm";
        case 101: return "no_drive";
        case 102: return "cal_cancel";
        case 103: return "prefs";
        case 104: return "scale_lost";
        case 105: return "no_output";
        case 106: return "no_scale";
        case 107: return "reboot_drv";
    }
    return "unknown";
}

bool decodeGaugeProbeFrameUm(uint32_t frame, int32_t* outUm) {
    if(outUm == nullptr) return false;

    const bool inchMode = (frame & (1UL << 23)) != 0;
    const bool negative = (frame & (1UL << 20)) != 0;
    const uint32_t counts = frame & 0x000FFFFFUL;
    int64_t um = 0;

    if(inchMode) {
        um = (static_cast<int64_t>(counts) * 635 + 50) / 100;
    } else {
        um = static_cast<int64_t>(counts) * 5;
    }

    if(negative) {
        um = -um;
    }

    if(um > kGaugeProbeMaxReasonableGapUm || um < -kGaugeProbeMaxReasonableGapUm) {
        return false;
    }

    *outUm = static_cast<int32_t>(um);
    return true;
}

bool gpioProbePinValid(uint32_t pin) {
    return pin < GPIO_NUM_MAX && pin != 19 && pin != 20;
}

bool defaultGaugeProbeMapping(uint8_t* clkPin, uint8_t* dataPin, const char** header) {
    const ControllerBoardBurrGapInput& input = controller_board_burr_gap_input();
    if(!controller_board_pin_valid(input.clock) || !controller_board_pin_valid(input.data)) {
        return false;
    }
    if(clkPin != nullptr) {
        *clkPin = static_cast<uint8_t>(input.clock);
    }
    if(dataPin != nullptr) {
        *dataPin = static_cast<uint8_t>(input.data);
    }
    if(header != nullptr) {
        *header = input.headerLabel != nullptr ? input.headerLabel : "controller burr gap GPIO mapping";
    }
    return true;
}

GaugeProbeResult runGaugeProbe(uint8_t clkPin, uint8_t dataPin, uint16_t durationMs) {
    GaugeProbeResult result;

    pinMode(clkPin, INPUT_PULLUP);
    pinMode(dataPin, INPUT_PULLUP);
    delayMicroseconds(100);

    uint8_t lastClk = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(clkPin)) != 0);
    uint8_t lastData = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(dataPin)) != 0);
    result.initialClk = lastClk;
    result.initialData = lastData;

    uint8_t bitIndex = 0;
    uint32_t shift = 0;
    uint32_t lastEdgeUs = 0;
    const uint32_t startUs = micros();
    const uint32_t durationUs = static_cast<uint32_t>(durationMs) * 1000UL;

    while(static_cast<uint32_t>(micros() - startUs) < durationUs) {
        const uint32_t nowUs = micros();
        const uint8_t clk = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(clkPin)) != 0);
        const uint8_t data = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(dataPin)) != 0);
        result.samples++;

        if(data != lastData) {
            result.dataTransitions++;
            lastData = data;
        }

        if(clk == lastClk) {
            continue;
        }

        result.clkTransitions++;
        if(clk != 0) {
            result.clkRising++;
        } else {
            result.clkFalling++;
            if((nowUs - lastEdgeUs) > kGaugeProbeFrameGapUs) {
                bitIndex = 0;
                shift = 0;
            }

            if(bitIndex < kGaugeProbeFrameBits) {
                if(data != 0) {
                    shift |= (1UL << bitIndex);
                }
                bitIndex++;
            }

            if(bitIndex >= kGaugeProbeFrameBits) {
                result.frames++;
                result.lastFrame = shift;
                int32_t um = 0;
                if(decodeGaugeProbeFrameUm(shift, &um)) {
                    result.validFrames++;
                    result.lastUm = um;
                }
                bitIndex = 0;
                shift = 0;
            }
            lastEdgeUs = nowUs;
        }

        lastClk = clk;
    }

    result.finalClk = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(clkPin)) != 0);
    result.finalData = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(dataPin)) != 0);
    pinMode(clkPin, INPUT_PULLUP);
    pinMode(dataPin, INPUT_PULLUP);
    return result;
}

const char* levelText(uint8_t level) {
    switch(level) {
        case ERROR_HISTORY_LEVEL_INFO: return "INFO";
        case ERROR_HISTORY_LEVEL_WARN: return "WARN";
        case ERROR_HISTORY_LEVEL_ERROR: return "ERR";
    }
    return "UNK";
}

const char* rpmSourceText(uint8_t source) {
    switch(source) {
        case ERROR_HISTORY_MOTOR_RPM_SRC_JMC_2114: return "J2114";
        case ERROR_HISTORY_MOTOR_RPM_SRC_JMC_606C: return "J606C";
        case ERROR_HISTORY_MOTOR_RPM_SRC_RT_1301: return "RT1301";
        case ERROR_HISTORY_MOTOR_RPM_SRC_KRS_DN008: return "KRS";
        case ERROR_HISTORY_MOTOR_RPM_SRC_SEW_PI2: return "SEWPI2";
        case ERROR_HISTORY_MOTOR_RPM_SRC_TSE_1000: return "TSE1000";
    }
    return "UNK";
}

uint8_t errorLevel(uint8_t code) {
    if(code >= 1 && code <= 8) {
        return ERROR_HISTORY_LEVEL_ERROR;
    }
    if(code >= 100) {
        return ERROR_HISTORY_LEVEL_WARN;
    }
    return ERROR_HISTORY_LEVEL_INFO;
}

uint8_t commLevel(COMM_STATUS value) {
    switch(value) {
        case COMM_CONNECTED: return ERROR_HISTORY_LEVEL_INFO;
        case COMM_DISCONNECTED: return ERROR_HISTORY_LEVEL_WARN;
        case COMM_INVALID_STATUS: return ERROR_HISTORY_LEVEL_WARN;
    }
    return ERROR_HISTORY_LEVEL_WARN;
}

uint8_t motorLevel(SYSTEM_STATUS value) {
    switch(value) {
        case MOTOR_READY: return ERROR_HISTORY_LEVEL_INFO;
        case MOTOR_ENABLED: return ERROR_HISTORY_LEVEL_INFO;
        case MOTOR_FAULT: return ERROR_HISTORY_LEVEL_ERROR;
        case MOTOR_NOT_CONNECTED: return ERROR_HISTORY_LEVEL_WARN;
        case MOTOR_NOT_READY: return ERROR_HISTORY_LEVEL_WARN;
        case MOTOR_INVALID: return ERROR_HISTORY_LEVEL_WARN;
    }
    return ERROR_HISTORY_LEVEL_WARN;
}

uint8_t scaleLevel(SCALE_STATUS value) {
    switch(value) {
        case SCALE_CONNECTED: return ERROR_HISTORY_LEVEL_INFO;
        case SCALE_CONNECTING: return ERROR_HISTORY_LEVEL_INFO;
        case SCALE_DISCONNECTED: return ERROR_HISTORY_LEVEL_WARN;
        case SCALE_LOST: return ERROR_HISTORY_LEVEL_ERROR;
        case INVALID_SCALE_STATUS: return ERROR_HISTORY_LEVEL_WARN;
    }
    return ERROR_HISTORY_LEVEL_WARN;
}

const menuEntry* menuByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        case 4: return Menu4;
        case 5: return Menu5;
        default: return nullptr;
    }
}

uint8_t menuCountByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return NUM_MENU1_ITEMS;
        case 2: return NUM_MENU2_ITEMS;
        case 3: return NUM_MENU3_ITEMS;
        case 4: return NUM_MENU4_ITEMS;
        case 5: return NUM_MENU5_ITEMS;
        default: return 0;
    }
}

int16_t gaugeMenuValueFromUm(int32_t gapUm) {
    int32_t steps = gapUm / 10;
    if(steps > Menu1[Menu1Items::GAUGESET].maxValue) steps = Menu1[Menu1Items::GAUGESET].maxValue;
    if(steps < Menu1[Menu1Items::GAUGESET].minValue) steps = Menu1[Menu1Items::GAUGESET].minValue;
    return static_cast<int16_t>(steps);
}

void syncGaugeMenuValue() {
    Menu1[Menu1Items::GAUGESET].value = gaugeMenuValueFromUm(burr_gap_base_lcd_um());
}

uint8_t visibleMenuCountByNumber(uint8_t menuNum) {
    const uint8_t count = menuCountByNumber(menuNum);
    uint8_t visibleCount = 0;
    for(uint8_t idx = 0; idx < count; ++idx) {
        if(motor_menu_item_visible(menuNum, idx)) {
            ++visibleCount;
        }
    }
    return visibleCount;
}

const char* menuLabel(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return "Main";
        case 2: return "Purge Settings";
        case 3: return "GbW Settings";
        case 4: return "JMC Baud";
        case 5: return "Wireless";
        default: return "Unknown";
    }
}

States menuStateByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return MENU1;
        case 2: return MENU2;
        case 3: return MENU3;
        case 4: return MENU4;
        case 5: return MENU5;
        default: return IDLE;
    }
}

int8_t* menuSelectionByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return &menu1Selected;
        case 2: return &menu2Selected;
        case 3: return &menu3Selected;
        case 4: return &menu4Selected;
        case 5: return &menu5Selected;
        default: return nullptr;
    }
}

uint8_t currentMenuNumber() {
    switch(state) {
        case MENU1: return 1;
        case MENU2: return 2;
        case MENU3: return 3;
        case MENU4: return 4;
        case MENU5: return 5;
        default: return 0;
    }
}

uint32_t baudForJmcMenuIndex(uint8_t selectedIdx) {
    switch(selectedIdx) {
        case Menu4Items::JMCBAUD9600: return 9600;
        case Menu4Items::JMCBAUD19200: return 19200;
        case Menu4Items::JMCBAUD38400: return 38400;
        case Menu4Items::JMCBAUD57600: return 57600;
        case Menu4Items::JMCBAUD115200: return 115200;
        default: return 0;
    }
}

bool shouldOpenDriveSelectorApi() {
    return motor_get_drive_type() == DRIVE_TYPE_UNKNOWN || error == 1;
}

int16_t clampMenuValue(const menuEntry& entry, int32_t requested) {
    int32_t clamped = requested;
    if(clamped < entry.minValue) clamped = entry.minValue;
    if(clamped > entry.maxValue) clamped = entry.maxValue;
    return static_cast<int16_t>(clamped);
}

void jsonPrintQuoted(const char* value) {
    Serial.print('"');
    if(value != nullptr) {
        for(const char* cursor = value; *cursor != '\0'; ++cursor) {
            switch(*cursor) {
                case '\\': Serial.print("\\\\"); break;
                case '"': Serial.print("\\\""); break;
                case '\n': Serial.print("\\n"); break;
                case '\r': Serial.print("\\r"); break;
                case '\t': Serial.print("\\t"); break;
                default: Serial.print(*cursor); break;
            }
        }
    }
    Serial.print('"');
}

void jsonPrintQuoted(const String& value) {
    jsonPrintQuoted(value.c_str());
}

String stableDeviceMacAddress() {
    const uint64_t mac = ESP.getEfuseMac();
    char out[18];
    snprintf(out,
             sizeof(out),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             static_cast<unsigned>(mac & 0xFF),
             static_cast<unsigned>((mac >> 8) & 0xFF),
             static_cast<unsigned>((mac >> 16) & 0xFF),
             static_cast<unsigned>((mac >> 24) & 0xFF),
             static_cast<unsigned>((mac >> 32) & 0xFF),
             static_cast<unsigned>((mac >> 40) & 0xFF));
    return String(out);
}

String runningApplicationOffsetHex() {
    const esp_partition_t* partition = esp_ota_get_running_partition();
    char out[12];
    snprintf(out, sizeof(out), "0x%lx", static_cast<unsigned long>(partition != nullptr ? partition->address : 0x10000));
    return String(out);
}

uint32_t runningApplicationSize() {
    const esp_partition_t* partition = esp_ota_get_running_partition();
    return partition != nullptr ? partition->size : 0;
}

String nextOtaOffsetHex() {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    char out[12];
    snprintf(out, sizeof(out), "0x%lx", static_cast<unsigned long>(partition != nullptr ? partition->address : 0));
    return String(out);
}

uint32_t nextOtaSize() {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    return partition != nullptr ? partition->size : 0;
}

bool apiCanStartOta() {
    switch(state) {
        case IDLE:
        case IDLE_GBW:
        case MENU1:
        case MENU2:
        case MENU3:
        case MENU4:
        case MENU5:
        case MOTOR_SELECT:
        case NETWORK_INFO:
        case ABOUT_INFO:
            return true;
        default:
            return false;
    }
}

void scheduleApiReboot() {
    pendingApiReboot = true;
    pendingApiRebootAt = millis() + kApiRebootDelayMs;
}

const esp_partition_t* otaBootPartitionForToken(const char* token) {
    if(token == nullptr) {
        return nullptr;
    }
    if(strcmp(token, "next") == 0) {
        return esp_ota_get_next_update_partition(nullptr);
    }
    if(strcmp(token, "running") == 0 || strcmp(token, "current") == 0) {
        return esp_ota_get_running_partition();
    }
    if(strcmp(token, "app0") == 0 || strcmp(token, "ota0") == 0 || strcmp(token, "ota_0") == 0) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    }
    if(strcmp(token, "app1") == 0 || strcmp(token, "ota1") == 0 || strcmp(token, "ota_1") == 0) {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    }
    return nullptr;
}

bool apiSetOtaBootPartition(const char* target, const char** message) {
    const esp_partition_t* partition = otaBootPartitionForToken(target);
    if(partition == nullptr) {
        *message = "usage: api ota boot <next|running|app0|app1> [reboot]";
        return false;
    }

    const esp_err_t err = esp_ota_set_boot_partition(partition);
    if(err != ESP_OK) {
        snprintf(otaMessageBuffer, sizeof(otaMessageBuffer), "set boot failed err=%d", static_cast<int>(err));
        *message = otaMessageBuffer;
        return false;
    }

    snprintf(otaMessageBuffer,
             sizeof(otaMessageBuffer),
             "boot=%s offset=0x%lx reboot required",
             partition->label,
             static_cast<unsigned long>(partition->address));
    *message = otaMessageBuffer;
    return true;
}

void restoreOtaWifi(const OtaWifiGuard& guard) {
    if(guard.wifiWasActive) {
        return;
    }
    wifi_ui_stop();
    if(guard.espNowWasActive && !espnow_control_is_active()) {
        (void)espnow_control_start(false);
    }
}

bool ensureOtaWifi(OtaWifiGuard* guard, String* message) {
    if(guard == nullptr) {
        return false;
    }
    guard->wifiWasActive = wifi_ui_is_active();
    guard->espNowWasActive = espnow_control_is_active();

    if(wifi_ui_sta_connected()) {
        return true;
    }
    if(!wifi_ui_has_saved_network()) {
        if(message != nullptr) {
            *message = "no saved wifi";
        }
        return false;
    }

    if(!guard->wifiWasActive && guard->espNowWasActive) {
        espnow_control_stop();
    }
    if(!wifi_ui_start()) {
        restoreOtaWifi(*guard);
        if(message != nullptr) {
            *message = "wifi start failed";
        }
        return false;
    }

    const uint32_t startedAt = millis();
    while(static_cast<uint32_t>(millis() - startedAt) < 30000UL) {
        wifi_ui_poll();
        if(wifi_ui_sta_connected()) {
            return true;
        }
        delay(50);
        yield();
    }

    restoreOtaWifi(*guard);
    if(message != nullptr) {
        *message = "wifi connect timeout";
    }
    return false;
}

bool apiDownloadOta(const char* url, String* message) {
    if(url == nullptr || url[0] == '\0') {
        if(message != nullptr) {
            *message = "usage: api ota download <url> [boot|reboot]";
        }
        return false;
    }

    OtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        return false;
    }

    sg98_ota_download::Result result;
    const bool ok = sg98_ota_download::downloadToNextSlot(String(url), &result);
    if(ok) {
        (void)controller_ota_note_downloaded(result);
    }
    restoreOtaWifi(guard);
    if(message != nullptr) {
        *message = result.message;
    }
    return ok;
}

bool apiDownloadOtaManifest(const char* url, String* message) {
    const String resolvedUrl = sg98_ota_source::resolveManifestUrl(url != nullptr ? String(url) : String());
    if(resolvedUrl.length() == 0) {
        if(message != nullptr) {
            *message = "usage: api ota manifest <url|latest> [boot|reboot]";
        }
        return false;
    }

    OtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        return false;
    }

    sg98_ota_download::Result result;
    const bool ok = sg98_ota_download::downloadManifestToNextSlot(resolvedUrl, &result);
    if(ok) {
        (void)controller_ota_note_downloaded(result);
    }
    restoreOtaWifi(guard);
    if(message != nullptr) {
        *message = result.message;
    }
    return ok;
}

bool apiCheckOtaManifest(const char* url, String* message) {
    const String resolvedUrl = sg98_ota_source::resolveManifestUrl(url != nullptr ? String(url) : String());
    if(resolvedUrl.length() == 0) {
        if(message != nullptr) {
            *message = "usage: api ota check [url|latest]";
        }
        return false;
    }

    OtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        return false;
    }

    const bool ok = controller_env_check_ota_manifest_now(resolvedUrl, message);
    restoreOtaWifi(guard);
    return ok;
}

bool apiDownloadOtaIfAvailable(const char* url, String* message) {
    const String resolvedUrl = sg98_ota_source::resolveManifestUrl(url != nullptr ? String(url) : String());
    if(resolvedUrl.length() == 0) {
        if(message != nullptr) {
            *message = "usage: api ota download_if_available [url|latest]";
        }
        return false;
    }

    OtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        return false;
    }

    const bool ok = controller_env_download_ota_if_available_now(resolvedUrl, message);
    restoreOtaWifi(guard);
    return ok;
}

void apiPrintDevice() {
    Serial.print("{\"type\":\"device\",\"protocol\":\"sg98-usb-device-v1\",\"role\":\"controller\",\"target\":");
    jsonPrintQuoted(controller_board_target_name());
    Serial.print(",\"board\":");
    jsonPrintQuoted(controller_board_display_name());
    const ControllerBoardBurrGapInput& burrGapInput = controller_board_burr_gap_input();
    const bool hasBurrGapInput = controller_board_has_burr_gap_input();
    Serial.print(",\"burrGapInput\":{\"enabled\":");
    Serial.print(hasBurrGapInput ? "true" : "false");
    Serial.print(",\"clockGpio\":");
    Serial.print(hasBurrGapInput ? static_cast<int>(burrGapInput.clock) : -1);
    Serial.print(",\"dataGpio\":");
    Serial.print(hasBurrGapInput ? static_cast<int>(burrGapInput.data) : -1);
    Serial.print(",\"mapping\":");
    jsonPrintQuoted(hasBurrGapInput && burrGapInput.headerLabel != nullptr ? burrGapInput.headerLabel : "");
    Serial.print("}");
    Serial.print(",\"mac\":");
    jsonPrintQuoted(stableDeviceMacAddress());
    Serial.print(",\"firmwareVersion\":");
    jsonPrintQuoted(sg98_version::release());
    Serial.print(",\"gitSha\":");
    jsonPrintQuoted(sg98_version::gitSha());
    Serial.print(",\"gitDirty\":");
    Serial.print(sg98_version::dirty() ? "true" : "false");
    Serial.print(",\"buildEnv\":");
    jsonPrintQuoted(sg98_version::buildEnv());
    Serial.print(",\"buildDate\":");
    jsonPrintQuoted(SG98_BUILD_DATE);
    Serial.print(",\"buildTime\":");
    jsonPrintQuoted(SG98_BUILD_TIME);
    Serial.print(",\"supportsAppOnlyUpdate\":true");
    Serial.print(",\"applicationOffset\":");
    jsonPrintQuoted(runningApplicationOffsetHex());
    Serial.print(",\"applicationSize\":");
    Serial.print(static_cast<unsigned long>(runningApplicationSize()));
    Serial.print(",\"supportsOtaUpdate\":");
    Serial.print(nextOtaSize() > 0 ? "true" : "false");
    Serial.print(",\"otaManifestUrl\":");
    jsonPrintQuoted(sg98_ota_source::latestManifestUrl());
    Serial.print(",\"otaNextOffset\":");
    jsonPrintQuoted(nextOtaOffsetHex());
    Serial.print(",\"otaNextSize\":");
    Serial.print(static_cast<unsigned long>(nextOtaSize()));
    Serial.print(",\"buildId\":");
    jsonPrintQuoted(BUILD_ID);
    Serial.println("}");
}

const char* apiMenuItemKind(uint8_t menuNum, uint8_t itemIdx) {
    if(!motor_menu_item_visible(menuNum, itemIdx)) {
        return "hidden";
    }

    if(menuNum == 1) {
        switch(itemIdx) {
            case Menu1Items::EXITMENU: return "nav";
            case Menu1Items::PURGESETTINGS:
            case Menu1Items::GBWSETTINGS:
            case Menu1Items::JMCBAUDMENU:
            case Menu1Items::RADIOMODE:
                return "submenu";
            case Menu1Items::DRIVESETTINGS:
            case Menu1Items::CALIBRATE:
            case Menu1Items::NETWORKINFO:
            case Menu1Items::ABOUT:
                return "action";
            case Menu1Items::RESET:
                return "danger_action";
            default:
                return (Menu1[itemIdx].minValue == 0 && Menu1[itemIdx].maxValue == 1 && Menu1[itemIdx].scalar == 1) ? "bool" : "value";
        }
    }

    if(menuNum == 2) {
        if(itemIdx == Menu2Items::RETURN_FROM_PURGE) return "nav";
        return (Menu2[itemIdx].minValue == 0 && Menu2[itemIdx].maxValue == 1 && Menu2[itemIdx].scalar == 1) ? "bool" : "value";
    }

    if(menuNum == 3) {
        if(itemIdx == Menu3Items::RETURN_FROM_GBW) return "nav";
        if(itemIdx == Menu3Items::SAVE_SCALE) return "bool";
        return (Menu3[itemIdx].minValue == 0 && Menu3[itemIdx].maxValue == 1 && Menu3[itemIdx].scalar == 1) ? "bool" : "value";
    }

    if(menuNum == 4) {
        if(itemIdx == Menu4Items::RETURN_FROM_JMC_BAUD) return "nav";
        return "baud_action";
    }

    if(menuNum == 5) {
        if(itemIdx == Menu5Items::RETURN_FROM_WIRELESS) return "nav";
        if(itemIdx == Menu5Items::WIRELESS_NETWORK_INFO) return "action";
        if(itemIdx == Menu5Items::WIRELESS_RESET_NETWORK) return "danger_action";
        if(itemIdx == Menu5Items::WIRELESS_RADIO_MODE) return "value";
    }

    return "unknown";
}

const char* apiMenuItemAction(uint8_t menuNum, uint8_t itemIdx) {
    if(!motor_menu_item_visible(menuNum, itemIdx)) {
        return nullptr;
    }

    if(menuNum == 1) {
        switch(itemIdx) {
            case Menu1Items::DRIVESETTINGS: return "drive_select";
            case Menu1Items::CALIBRATE: return "calibrate";
            case Menu1Items::NETWORKINFO: return "network_info";
            case Menu1Items::ABOUT: return "about";
            case Menu1Items::RESET: return "reset_defaults";
            default: return nullptr;
        }
    }

    if(menuNum == 5) {
        switch(itemIdx) {
            case Menu5Items::WIRELESS_NETWORK_INFO: return "network_info";
            case Menu5Items::WIRELESS_RESET_NETWORK: return "reset_network";
            default: return nullptr;
        }
    }

    if(menuNum == 4 && itemIdx >= Menu4Items::JMCBAUD9600 && itemIdx <= Menu4Items::JMCBAUD115200) {
        return "jmc_baud";
    }

    return nullptr;
}

uint8_t apiTargetMenu(uint8_t menuNum, uint8_t itemIdx) {
    if(!motor_menu_item_visible(menuNum, itemIdx)) {
        return 0;
    }

    if(menuNum == 1) {
        switch(itemIdx) {
            case Menu1Items::PURGESETTINGS: return 2;
            case Menu1Items::GBWSETTINGS: return 3;
            case Menu1Items::JMCBAUDMENU: return 4;
            case Menu1Items::RADIOMODE: return 5;
            default: return 0;
        }
    }
    return 0;
}

bool apiMenuItemWritable(uint8_t menuNum, uint8_t itemIdx) {
    const char* kind = apiMenuItemKind(menuNum, itemIdx);
    return strcmp(kind, "value") == 0 || strcmp(kind, "bool") == 0;
}

void apiPrintAck(const char* cmd, bool ok, const char* message = nullptr) {
    Serial.print("{\"type\":\"ack\",\"cmd\":");
    jsonPrintQuoted(cmd);
    Serial.print(",\"ok\":");
    Serial.print(ok ? "true" : "false");
    if(message != nullptr) {
        Serial.print(",\"message\":");
        jsonPrintQuoted(message);
    }
    Serial.println("}");
}

void apiPrintGaugeProbe(uint16_t durationMs, uint8_t clkPin, uint8_t dataPin) {
    const GaugeProbeResult result = runGaugeProbe(clkPin, dataPin, durationMs);
    Serial.print("{\"type\":\"gauge_probe\",\"durationMs\":");
    Serial.print(static_cast<unsigned>(durationMs));
    Serial.print(",\"clkGpio\":");
    Serial.print(static_cast<unsigned>(clkPin));
    Serial.print(",\"dataGpio\":");
    Serial.print(static_cast<unsigned>(dataPin));
    Serial.print(",\"samples\":");
    Serial.print(static_cast<unsigned long>(result.samples));
    Serial.print(",\"clkInitial\":");
    Serial.print(static_cast<unsigned>(result.initialClk));
    Serial.print(",\"clkFinal\":");
    Serial.print(static_cast<unsigned>(result.finalClk));
    Serial.print(",\"dataInitial\":");
    Serial.print(static_cast<unsigned>(result.initialData));
    Serial.print(",\"dataFinal\":");
    Serial.print(static_cast<unsigned>(result.finalData));
    Serial.print(",\"clkTransitions\":");
    Serial.print(static_cast<unsigned long>(result.clkTransitions));
    Serial.print(",\"clkRising\":");
    Serial.print(static_cast<unsigned long>(result.clkRising));
    Serial.print(",\"clkFalling\":");
    Serial.print(static_cast<unsigned long>(result.clkFalling));
    Serial.print(",\"dataTransitions\":");
    Serial.print(static_cast<unsigned long>(result.dataTransitions));
    Serial.print(",\"frames\":");
    Serial.print(static_cast<unsigned long>(result.frames));
    Serial.print(",\"validFrames\":");
    Serial.print(static_cast<unsigned long>(result.validFrames));
    Serial.print(",\"lastFrame\":\"0x");
    char frameHex[9];
    snprintf(frameHex, sizeof(frameHex), "%06lX", static_cast<unsigned long>(result.lastFrame & 0x00FFFFFFUL));
    Serial.print(frameHex);
    Serial.print("\",\"lastUm\":");
    Serial.print(static_cast<long>(result.lastUm));
    Serial.print(",\"mappedHeader\":");
    const char* header = nullptr;
    uint8_t mappedClk = 0;
    uint8_t mappedData = 0;
    const bool matchesMapping =
        defaultGaugeProbeMapping(&mappedClk, &mappedData, &header) &&
        mappedClk == clkPin &&
        mappedData == dataPin;
    jsonPrintQuoted(matchesMapping && header != nullptr ? header : "custom GPIOs");
    Serial.println("}");
}

void apiPrintGaugeProbeAuto(uint16_t durationMs) {
    uint8_t pinA = 0;
    uint8_t pinB = 0;
    const char* header = nullptr;
    if(!defaultGaugeProbeMapping(&pinA, &pinB, &header)) {
        apiPrintAck("gauge_probe", false, "no burr gap GPIO mapping for this controller");
        return;
    }

    const GaugeProbeResult aClk = runGaugeProbe(pinA, pinB, durationMs);
    const GaugeProbeResult bClk = runGaugeProbe(pinB, pinA, durationMs);
    const bool preferA =
        aClk.validFrames > bClk.validFrames ||
        (aClk.validFrames == bClk.validFrames && aClk.frames > bClk.frames) ||
        (aClk.validFrames == bClk.validFrames && aClk.frames == bClk.frames && aClk.clkFalling > bClk.clkFalling);

    Serial.print("{\"type\":\"gauge_probe_auto\",\"durationMs\":");
    Serial.print(static_cast<unsigned>(durationMs));
    Serial.print(",\"mappedHeader\":");
    jsonPrintQuoted(header);
    Serial.print(",\"candidateA\":{\"clkGpio\":");
    Serial.print(static_cast<unsigned>(pinA));
    Serial.print(",\"dataGpio\":");
    Serial.print(static_cast<unsigned>(pinB));
    Serial.print(",\"clkTransitions\":");
    Serial.print(static_cast<unsigned long>(aClk.clkTransitions));
    Serial.print(",\"dataTransitions\":");
    Serial.print(static_cast<unsigned long>(aClk.dataTransitions));
    Serial.print(",\"frames\":");
    Serial.print(static_cast<unsigned long>(aClk.frames));
    Serial.print(",\"validFrames\":");
    Serial.print(static_cast<unsigned long>(aClk.validFrames));
    Serial.print(",\"lastFrame\":\"0x");
    char frameHex[9];
    snprintf(frameHex, sizeof(frameHex), "%06lX", static_cast<unsigned long>(aClk.lastFrame & 0x00FFFFFFUL));
    Serial.print(frameHex);
    Serial.print("\",\"lastUm\":");
    Serial.print(static_cast<long>(aClk.lastUm));
    Serial.print("},\"candidateB\":{\"clkGpio\":");
    Serial.print(static_cast<unsigned>(pinB));
    Serial.print(",\"dataGpio\":");
    Serial.print(static_cast<unsigned>(pinA));
    Serial.print(",\"clkTransitions\":");
    Serial.print(static_cast<unsigned long>(bClk.clkTransitions));
    Serial.print(",\"dataTransitions\":");
    Serial.print(static_cast<unsigned long>(bClk.dataTransitions));
    Serial.print(",\"frames\":");
    Serial.print(static_cast<unsigned long>(bClk.frames));
    Serial.print(",\"validFrames\":");
    Serial.print(static_cast<unsigned long>(bClk.validFrames));
    Serial.print(",\"lastFrame\":\"0x");
    snprintf(frameHex, sizeof(frameHex), "%06lX", static_cast<unsigned long>(bClk.lastFrame & 0x00FFFFFFUL));
    Serial.print(frameHex);
    Serial.print("\",\"lastUm\":");
    Serial.print(static_cast<long>(bClk.lastUm));
    Serial.print("},\"recommended\":\"");
    if(preferA) {
        Serial.print("A");
    } else {
        Serial.print("B");
    }
    Serial.print("\",\"recommendedClkGpio\":");
    Serial.print(static_cast<unsigned>(preferA ? pinA : pinB));
    Serial.print(",\"recommendedDataGpio\":");
    Serial.print(static_cast<unsigned>(preferA ? pinB : pinA));
    Serial.println("}");
}

void apiPrintStatus() {
    String connectedName;
    String connectedMac;
    String savedName;
    String savedMac;

    if(scaleMutex != NULL && xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(25))) {
        connectedName = scale_name;
        connectedMac = scale_mac;
        savedName = scale_connect_name;
        savedMac = scale_connect_mac;
        xSemaphoreGive(scaleMutex);
    }

    Serial.print("{\"type\":\"status\",\"state\":");
    jsonPrintQuoted(stateText(state));
    Serial.print(",\"drive\":");
    jsonPrintQuoted(motor_get_type_label());
    Serial.print(",\"uart\":");
    jsonPrintQuoted(comm_get_profile_label());
    Serial.print(",\"rs485Port\":");
    jsonPrintQuoted(comm_get_rs485_port_label(comm_get_rs485_port()));
    Serial.print(",\"jmcBaud\":");
    Serial.print(static_cast<unsigned long>(comm_get_jmc_baud()));
    Serial.print(",\"jmcPreferredBaud\":");
    Serial.print(static_cast<unsigned long>(motor_get_preferred_jmc_baud()));
    Serial.print(",\"comm\":");
    jsonPrintQuoted(commText(commStatus));
    Serial.print(",\"motor\":");
    jsonPrintQuoted(motorText(currentStatus));
    Serial.print(",\"scale\":");
    jsonPrintQuoted(scaleText(scaleStatus));
    Serial.print(",\"radioMode\":");
    jsonPrintQuoted(radio_mode_label(radio_mode_current()));
    Serial.print(",\"radioDesiredMode\":");
    jsonPrintQuoted(radio_mode_label(radio_mode_desired()));
    Serial.print(",\"radioStatus\":");
    jsonPrintQuoted(radio_mode_status_text());
    Serial.print(",\"apStations\":");
    Serial.print(static_cast<unsigned>(WiFi.softAPgetStationNum()));
    Serial.print(",\"radioRecovery\":");
    Serial.print(radio_mode_web_recovery_active() ? "true" : "false");
    Serial.print(",\"radioRecoveryMs\":");
    Serial.print(static_cast<unsigned long>(radio_mode_recovery_remaining_ms()));
    Serial.print(",\"firmwareVersion\":");
    jsonPrintQuoted(sg98_version::release());
    Serial.print(",\"firmwareGitSha\":");
    jsonPrintQuoted(sg98_version::gitSha());
    Serial.print(",\"firmwareDirty\":");
    Serial.print(sg98_version::dirty() ? "true" : "false");
    Serial.print(",\"buildEnv\":");
    jsonPrintQuoted(sg98_version::buildEnv());
    Serial.print(",\"remoteRecoveryRequired\":");
    Serial.print(controller_board_requires_remote_recovery() ? "true" : "false");
    Serial.print(",\"error\":");
    Serial.print(static_cast<unsigned>(error));
    Serial.print(",\"errorText\":");
    jsonPrintQuoted(errorText(error));
    Serial.print(",\"setRPM\":");
    Serial.print(setRPM);
    Serial.print(",\"motorSetRPM\":");
    Serial.print(motor_get_effective_set_rpm());
    Serial.print(",\"currentRPM\":");
    Serial.print(motor_currentRPM);
    Serial.print(",\"torque\":");
    Serial.print(motor_currentTorque);
    Serial.print(",\"torquePercent\":");
    Serial.print(motor_get_torque_percent());
    Serial.print(",\"burrGapEnabled\":");
    Serial.print(burr_gap_enabled() ? "true" : "false");
    Serial.print(",\"burrGapKnown\":");
    Serial.print(burr_gap_has_value() ? "true" : "false");
    Serial.print(",\"burrGapLive\":");
    Serial.print(burr_gap_live() ? "true" : "false");
    Serial.print(",\"burrGapManualBase\":");
    Serial.print(burr_gap_manual_base_set() ? "true" : "false");
    Serial.print(",\"burrGapUm\":");
    Serial.print(static_cast<long>(burr_gap_display_lcd_um()));
    Serial.print(",\"burrGapRawUm\":");
    Serial.print(static_cast<long>(burr_gap_raw_um()));
    Serial.print(",\"burrGapBaseUm\":");
    Serial.print(static_cast<long>(burr_gap_base_lcd_um()));
    Serial.print(",\"burrGapOffsetUm\":");
    Serial.print(static_cast<long>(burr_gap_offset_um()));
    Serial.print(",\"setWeight\":");
    Serial.print(static_cast<unsigned long>(setWeight));
    Serial.print(",\"weight\":");
    Serial.print(static_cast<long>(currentWeight));
    Serial.print(",\"currentMenu\":");
    Serial.print(static_cast<unsigned>(currentMenuNumber()));
    Serial.print(",\"enterMenu\":");
    Serial.print(enterMenu ? "true" : "false");
    Serial.print(",\"savedScaleName\":");
    jsonPrintQuoted(savedName);
    Serial.print(",\"savedScaleMac\":");
    jsonPrintQuoted(savedMac);
    Serial.print(",\"seenScaleName\":");
    jsonPrintQuoted(connectedName);
    Serial.print(",\"seenScaleMac\":");
    jsonPrintQuoted(connectedMac);
    Serial.println("}");
}

void apiPrintMenu(uint8_t menuNum) {
    if(menuNum == 1) {
        syncGaugeMenuValue();
    }

    const menuEntry* menu = menuByNumber(menuNum);
    if(menu == nullptr) {
        return;
    }

    const uint8_t count = menuCountByNumber(menuNum);
    const int8_t* selectedPtr = menuSelectionByNumber(menuNum);
    const uint8_t activeMenu = currentMenuNumber();
    const uint8_t visibleCount = visibleMenuCountByNumber(menuNum);

    Serial.print("{\"type\":\"menu\",\"menu\":");
    Serial.print(static_cast<unsigned>(menuNum));
    Serial.print(",\"label\":");
    jsonPrintQuoted(menuLabel(menuNum));
    Serial.print(",\"count\":");
    Serial.print(static_cast<unsigned>(visibleCount));
    Serial.print(",\"active\":");
    Serial.print(activeMenu == menuNum ? "true" : "false");
    Serial.print(",\"selected\":");
    Serial.print(selectedPtr != nullptr ? static_cast<int>(*selectedPtr) : -1);
    Serial.println("}");

    for(uint8_t idx = 0; idx < count; ++idx) {
        if(!motor_menu_item_visible(menuNum, idx)) {
            continue;
        }
        const menuEntry& entry = menu[idx];
        const char* kind = apiMenuItemKind(menuNum, idx);
        Serial.print("{\"type\":\"item\",\"menu\":");
        Serial.print(static_cast<unsigned>(menuNum));
        Serial.print(",\"index\":");
        Serial.print(static_cast<unsigned>(idx));
        Serial.print(",\"name\":");
        jsonPrintQuoted(entry.name);
        Serial.print(",\"kind\":");
        jsonPrintQuoted(kind);
        Serial.print(",\"writable\":");
        Serial.print(apiMenuItemWritable(menuNum, idx) ? "true" : "false");
        Serial.print(",\"value\":");
        Serial.print(entry.value);
        Serial.print(",\"min\":");
        Serial.print(entry.minValue);
        Serial.print(",\"max\":");
        Serial.print(entry.maxValue);
        Serial.print(",\"scalar\":");
        Serial.print(entry.scalar);
        Serial.print(",\"unit\":");
        jsonPrintQuoted(entry.unit);
        Serial.print(",\"selected\":");
        Serial.print(activeMenu == menuNum && selectedPtr != nullptr && *selectedPtr == static_cast<int8_t>(idx) ? "true" : "false");

        const uint8_t targetMenu = apiTargetMenu(menuNum, idx);
        if(targetMenu != 0) {
            Serial.print(",\"targetMenu\":");
            Serial.print(static_cast<unsigned>(targetMenu));
        }

        const char* action = apiMenuItemAction(menuNum, idx);
        if(action != nullptr) {
            Serial.print(",\"action\":");
            jsonPrintQuoted(action);
        }

        const uint32_t baud = (menuNum == 4) ? baudForJmcMenuIndex(idx) : 0;
        if(baud != 0) {
            Serial.print(",\"baud\":");
            Serial.print(static_cast<unsigned long>(baud));
            Serial.print(",\"activeBaud\":");
            const bool activeBaud =
                motor_get_drive_type() == DRIVE_TYPE_JMC &&
                commStatus == COMM_CONNECTED &&
                comm_get_jmc_baud() == baud;
            Serial.print(activeBaud ? "true" : "false");
        }

        Serial.println("}");
    }
}

void apiPrintSnapshot() {
    Serial.println("{\"type\":\"snapshot_begin\"}");
    apiPrintStatus();
    apiPrintMenu(1);
    apiPrintMenu(2);
    apiPrintMenu(3);
    apiPrintMenu(4);
    apiPrintMenu(5);
    Serial.println("{\"type\":\"snapshot_end\"}");
}

bool persistMenuValue(uint8_t menuNum, uint8_t itemIdx) {
    const States originalState = state;
    const int8_t originalMenu1 = menu1Selected;
    const int8_t originalMenu2 = menu2Selected;
    const int8_t originalMenu3 = menu3Selected;
    const int8_t originalMenu4 = menu4Selected;

    state = menuStateByNumber(menuNum);
    if(int8_t* selectedPtr = menuSelectionByNumber(menuNum)) {
        *selectedPtr = static_cast<int8_t>(itemIdx);
    }

    pdata_write(2);
    pdata_normalize();

    state = originalState;
    menu1Selected = originalMenu1;
    menu2Selected = originalMenu2;
    menu3Selected = originalMenu3;
    menu4Selected = originalMenu4;
    return true;
}

bool persistSetValue(States persistState) {
    const States originalState = state;
    state = persistState;
    pdata_write(3);
    pdata_normalize();
    state = originalState;
    return true;
}

bool persistScaleSavePreference(bool saveScale) {
    const States originalState = state;
    const int8_t originalMenu3 = menu3Selected;

    state = MENU3;
    menu3Selected = Menu3Items::SAVE_SCALE;
    Menu3[Menu3Items::SAVE_SCALE].value = saveScale ? 1 : 0;

    if(!saveScale) {
        pdata_write(6);
        if(xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            scale_connect_mac = "";
            scale_connect_name = "";
            xSemaphoreGive(scaleMutex);
        }
    } else {
        if(xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            if(scale_mac != "") {
                scale_connect_mac = scale_mac;
                scale_connect_name = scale_name;
            }
            xSemaphoreGive(scaleMutex);
        }
        pdata_write(7);
    }

    pdata_normalize();

    state = originalState;
    menu3Selected = originalMenu3;
    return true;
}

bool apiSetMenuValue(uint8_t menuNum, uint8_t itemIdx, int32_t requestedValue, const char** message) {
    const menuEntry* menu = menuByNumber(menuNum);
    const uint8_t count = menuCountByNumber(menuNum);
    if(menu == nullptr || itemIdx >= count) {
        *message = "invalid menu item";
        return false;
    }

    if(!motor_menu_item_visible(menuNum, itemIdx)) {
        *message = "item unavailable for active drive";
        return false;
    }

    const bool legacyRadioModeWrite = menuNum == 1 && itemIdx == Menu1Items::RADIOMODE;
    if(!legacyRadioModeWrite && !apiMenuItemWritable(menuNum, itemIdx)) {
        *message = "item is not writable";
        return false;
    }

    if(menuNum == 1) {
        const int16_t originalValue = Menu1[itemIdx].value;
        int32_t valueToWrite = requestedValue;
        if(itemIdx == Menu1Items::SETMOTORDIRECTION &&
           !(state == IDLE || state == IDLE_GBW || state == MENU1)) {
            *message = "motor direction can only change while idle";
            return false;
        }
        if(itemIdx == Menu1Items::RADIOMODE) {
            const RadioMode requestedMode = radio_mode_from_value(requestedValue);
            if(controller_board_requires_remote_recovery() && requestedMode == RADIO_MODE_OFF) {
                *message = "radio off is unavailable on headless controller";
                return false;
            }
            valueToWrite = radio_mode_sanitize_value(requestedValue);
        }
        Menu1[itemIdx].value = clampMenuValue(Menu1[itemIdx], valueToWrite);
        if(itemIdx == Menu1Items::SETMAX && Menu1[SETMAX].value < Menu1[SETMIN].value) {
            Menu1[SETMAX].value = Menu1[SETMIN].value;
        }
        if(itemIdx == Menu1Items::SETMIN && Menu1[SETMIN].value > Menu1[SETMAX].value) {
            Menu1[SETMIN].value = Menu1[SETMAX].value;
        }
        if(itemIdx == Menu1Items::RADIOMODE) {
            if(!radio_mode_can_switch_now()) {
                Menu1[itemIdx].value = originalValue;
                *message = "radio mode can only switch while idle";
                return false;
            }
            if(!radio_mode_apply()) {
                Menu1[itemIdx].value = originalValue;
                *message = "radio mode switch failed";
                return false;
            }
        }
        if(itemIdx == Menu1Items::GAUGESET) {
            burr_gap_set_base_um(static_cast<int32_t>(Menu1[itemIdx].value) * 10);
        } else {
            persistMenuValue(menuNum, itemIdx);
        }
    } else if(menuNum == 2) {
        Menu2[itemIdx].value = clampMenuValue(Menu2[itemIdx], requestedValue);
        persistMenuValue(menuNum, itemIdx);
    } else if(menuNum == 3) {
        Menu3[itemIdx].value = clampMenuValue(Menu3[itemIdx], requestedValue);
        if(itemIdx == Menu3Items::GBW_RPM_SET) {
            int16_t slowLimit = Menu3[GBW_SLOW_RPM].value;
            if(slowLimit > Menu1[SETMAX].value) slowLimit = Menu1[SETMAX].value;
            if(Menu3[GBW_RPM_SET].value > Menu1[SETMAX].value) Menu3[GBW_RPM_SET].value = Menu1[SETMAX].value;
            if(Menu3[GBW_RPM_SET].value < slowLimit) Menu3[GBW_RPM_SET].value = slowLimit;
        }
        if(itemIdx == Menu3Items::GBW_SLOW_RPM) {
            if(Menu3[GBW_SLOW_RPM].value > Menu1[SETMAX].value) Menu3[GBW_SLOW_RPM].value = Menu1[SETMAX].value;
            if(Menu3[GBW_SLOW_RPM].value > Menu3[GBW_RPM_SET].value) Menu3[GBW_SLOW_RPM].value = Menu3[GBW_RPM_SET].value;
        }
        if(itemIdx == Menu3Items::SAVE_SCALE) {
            persistScaleSavePreference(Menu3[itemIdx].value != 0);
        } else {
            persistMenuValue(menuNum, itemIdx);
        }
    } else if(menuNum == 5) {
        if(itemIdx != Menu5Items::WIRELESS_RADIO_MODE) {
            *message = "menu item is not writable";
            return false;
        }
        const int16_t originalValue = Menu1[Menu1Items::RADIOMODE].value;
        const RadioMode requestedMode = radio_mode_from_value(requestedValue);
        if(controller_board_requires_remote_recovery() && requestedMode == RADIO_MODE_OFF) {
            *message = "radio off is unavailable on headless controller";
            return false;
        }
        Menu5[itemIdx].value = clampMenuValue(Menu5[itemIdx], radio_mode_sanitize_value(requestedValue));
        Menu1[Menu1Items::RADIOMODE].value = Menu5[itemIdx].value;
        if(!radio_mode_can_switch_now()) {
            Menu1[Menu1Items::RADIOMODE].value = originalValue;
            Menu5[itemIdx].value = originalValue;
            *message = "radio mode can only switch while idle";
            return false;
        }
        if(!radio_mode_apply()) {
            Menu1[Menu1Items::RADIOMODE].value = originalValue;
            Menu5[itemIdx].value = originalValue;
            *message = "radio mode switch failed";
            return false;
        }
        persistMenuValue(1, Menu1Items::RADIOMODE);
    } else {
        *message = "menu does not accept direct value writes";
        return false;
    }

    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiSetRpmValue(int32_t requestedValue, const char** message) {
    setRPM = requestedValue;
    if(setRPM > Menu1[SETMAX].value) setRPM = Menu1[SETMAX].value;
    if(setRPM < Menu1[SETMIN].value) setRPM = Menu1[SETMIN].value;
    persistSetValue(IDLE);
    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiSetWeightValue(int32_t requestedValue, const char** message) {
    if(requestedValue < static_cast<int32_t>(default_min_weight)) requestedValue = default_min_weight;
    if(requestedValue > static_cast<int32_t>(default_max_weight)) requestedValue = default_max_weight;
    setWeight = static_cast<uint32_t>(requestedValue);
    persistSetValue(IDLE_GBW);
    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiActionStart(const char** message) {
    if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
        motor_ensure_drive_detected(true);
    }

    if(shouldOpenDriveSelectorApi()) {
        *message = "drive selection required";
        return false;
    }

    if(commStatus != COMM_CONNECTED || currentStatus == MOTOR_NOT_CONNECTED) {
        const uint32_t connectDeadline = millis() + 1200UL;
        while((commStatus != COMM_CONNECTED || currentStatus == MOTOR_NOT_CONNECTED) &&
              static_cast<int32_t>(connectDeadline - millis()) > 0) {
            do_comm();
            delay(5);
        }
    }

    if(commStatus != COMM_CONNECTED || currentStatus == MOTOR_NOT_CONNECTED) {
        *message = "motor not connected";
        return false;
    }

    if(currentStatus == MOTOR_FAULT) {
        *message = "motor fault";
        return false;
    }

    if(state == IDLE) {
        purge_prepare_for_grinding();
        state = GRINDING;
    } else if(state == IDLE_GBW) {
        state = GRINDING_GBW;
    } else {
        *message = "start only works from idle states";
        return false;
    }

    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiActionStop(const char** message) {
    if(state == GRINDING) {
        purge_mark_started();
        state = (Menu2[SETBUTTONPURGE].value == true ? PURGING : IDLE);
        if(state == IDLE) purge_cancel_pending();
    } else if(state == GRINDING_GBW) {
        state = IDLE_GBW;
        motor_setRPM = 0;
    } else if(state == PURGING) {
        purge_cancel_pending();
        state = IDLE;
        motor_setRPM = 0;
    } else {
        *message = "stop only works while running";
        return false;
    }

    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiActionIdle(States nextState, const char** message) {
    enterMenu = false;
    menu1Selected = EXITMENU;
    menu2Selected = RETURN_FROM_PURGE;
    menu3Selected = RETURN_FROM_GBW;
    menu4Selected = RETURN_FROM_JMC_BAUD;
    menu5Selected = RETURN_FROM_WIRELESS;
    motor_setRPM = 0;
    state = nextState;
    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiActionDrive(const char* mode, const char** message) {
    if(mode == nullptr) {
        *message = "missing drive mode";
        return false;
    }

    if(strcmp(mode, "auto") == 0) {
        pdata_write_drive_type(DRIVE_TYPE_UNKNOWN);
        motor_redetect_drive(false);
    } else if(strcmp(mode, "jmc") == 0) {
        motor_force_drive_type(DRIVE_TYPE_JMC, true);
    } else if(strcmp(mode, "rt") == 0) {
        motor_force_drive_type(DRIVE_TYPE_RT, true);
    } else if(strcmp(mode, "krs") == 0 || strcmp(mode, "krs130st-m07725") == 0) {
        motor_force_drive_type(DRIVE_TYPE_KRS_130ST_M07725, true);
    } else if(strcmp(mode, "xb130") == 0 || strcmp(mode, "krs-xb130aea20025-shl") == 0) {
        motor_force_drive_type(DRIVE_TYPE_KRS_XB130AEA20025_SHL, true);
    } else if(strcmp(mode, "sew") == 0 || strcmp(mode, "sew-movilink") == 0 || strcmp(mode, "movilink") == 0) {
        motor_force_drive_type(DRIVE_TYPE_SEW_MOVILINK, true);
    } else if(strcmp(mode, "tse") == 0) {
        motor_force_drive_type(DRIVE_TYPE_TSE, true);
    } else {
        *message = "invalid drive mode";
        return false;
    }

    error = 0;
    errorCode = 0;
    commCounter = 0;
    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiActionCalibrate(const char** message) {
    if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
        *message = "drive not detected";
        return false;
    }

    if(commStatus != COMM_CONNECTED || currentStatus == MOTOR_NOT_CONNECTED) {
        *message = "motor not connected";
        return false;
    }

    if(currentStatus == MOTOR_FAULT) {
        *message = "motor fault";
        return false;
    }

    state = CALIBRATING;
    enterMenu = false;
    controller_note_activity();
    *message = "ok";
    return true;
}

bool apiActionResetDefaults(const char* confirmToken, const char** message) {
    if(confirmToken == nullptr || strcmp(confirmToken, "confirm") != 0) {
        *message = "confirmation required";
        return false;
    }

    Menu1[Menu1Items::RESET].value = 1;
    pdata_write(8);
    motorReset();
    delay(1000);
    if(motor_get_drive_type() == DRIVE_TYPE_JMC) sleep_init();
    else ESP.restart();

    *message = "rebooting";
    return true;
}

bool apiActionResetNetwork(const char** message) {
    wifi_ui_reset_network_settings();
    controller_note_activity();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiActionNetworkInfo(const char** message) {
    if(!controller_board_has_display()) {
        *message = "display unavailable";
        return false;
    }
    state = NETWORK_INFO;
    enterMenu = false;
    controller_note_activity();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiActionAbout(const char** message) {
    if(!controller_board_has_display()) {
        *message = "display unavailable";
        return false;
    }
    state = ABOUT_INFO;
    enterMenu = false;
    controller_note_activity();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

void appendJsonQuoted(String& out, const char* value) {
    out += '"';
    if(value != nullptr) {
        for(const char* cursor = value; *cursor != '\0'; ++cursor) {
            switch(*cursor) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += *cursor; break;
            }
        }
    }
    out += '"';
}

void appendJsonQuoted(String& out, const String& value) {
    appendJsonQuoted(out, value.c_str());
}

void appendStatusJson(String& out) {
    String connectedName;
    String connectedMac;
    String savedName;
    String savedMac;

    if(scaleMutex != NULL && xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(25))) {
        connectedName = scale_name;
        connectedMac = scale_mac;
        savedName = scale_connect_name;
        savedMac = scale_connect_mac;
        xSemaphoreGive(scaleMutex);
    }

    out += '{';
    out += "\"state\":";
    appendJsonQuoted(out, stateText(state));
    out += ",\"drive\":";
    appendJsonQuoted(out, motor_get_type_label());
    out += ",\"uart\":";
    appendJsonQuoted(out, comm_get_profile_label());
    out += ",\"jmcBaud\":";
    out += String(static_cast<unsigned long>(comm_get_jmc_baud()));
    out += ",\"jmcPreferredBaud\":";
    out += String(static_cast<unsigned long>(motor_get_preferred_jmc_baud()));
    out += ",\"comm\":";
    appendJsonQuoted(out, commText(commStatus));
    out += ",\"motor\":";
    appendJsonQuoted(out, motorText(currentStatus));
    out += ",\"scale\":";
    appendJsonQuoted(out, scaleText(scaleStatus));
    out += ",\"radioMode\":";
    appendJsonQuoted(out, radio_mode_label(radio_mode_current()));
    out += ",\"radioDesiredMode\":";
    appendJsonQuoted(out, radio_mode_label(radio_mode_desired()));
    out += ",\"radioStatus\":";
    appendJsonQuoted(out, radio_mode_status_text());
    out += ",\"apStations\":";
    out += String(static_cast<unsigned>(WiFi.softAPgetStationNum()));
    out += ",\"radioRecovery\":";
    out += radio_mode_web_recovery_active() ? "true" : "false";
    out += ",\"radioRecoveryMs\":";
    out += String(static_cast<unsigned long>(radio_mode_recovery_remaining_ms()));
    out += ",\"firmwareVersion\":";
    appendJsonQuoted(out, sg98_version::release());
    out += ",\"firmwareGitSha\":";
    appendJsonQuoted(out, sg98_version::gitSha());
    out += ",\"firmwareDirty\":";
    out += sg98_version::dirty() ? "true" : "false";
    out += ",\"buildEnv\":";
    appendJsonQuoted(out, sg98_version::buildEnv());
    out += ",\"remoteRecoveryRequired\":";
    out += controller_board_requires_remote_recovery() ? "true" : "false";
    out += ",\"controllerOtaReady\":";
    out += controller_ota_ready_for_remote() ? "true" : "false";
    out += ",\"controllerOtaReadyDetail\":";
    appendJsonQuoted(out, controller_ota_ready_detail());
    out += ",\"error\":";
    out += String(static_cast<unsigned>(error));
    out += ",\"errorText\":";
    appendJsonQuoted(out, errorText(error));
    out += ",\"setRPM\":";
    out += String(setRPM);
    out += ",\"motorSetRPM\":";
    out += String(motor_get_effective_set_rpm());
    out += ",\"currentRPM\":";
    out += String(motor_currentRPM);
    out += ",\"torque\":";
    out += String(motor_currentTorque);
    out += ",\"torquePercent\":";
    out += String(motor_get_torque_percent());
    out += ",\"burrGapEnabled\":";
    out += burr_gap_enabled() ? "true" : "false";
    out += ",\"burrGapKnown\":";
    out += burr_gap_has_value() ? "true" : "false";
    out += ",\"burrGapLive\":";
    out += burr_gap_live() ? "true" : "false";
    out += ",\"burrGapManualBase\":";
    out += burr_gap_manual_base_set() ? "true" : "false";
    out += ",\"burrGapUm\":";
    out += String(static_cast<long>(burr_gap_display_lcd_um()));
    out += ",\"burrGapRawUm\":";
    out += String(static_cast<long>(burr_gap_raw_um()));
    out += ",\"burrGapBaseUm\":";
    out += String(static_cast<long>(burr_gap_base_lcd_um()));
    out += ",\"burrGapOffsetUm\":";
    out += String(static_cast<long>(burr_gap_offset_um()));
    out += ",\"setWeight\":";
    out += String(static_cast<unsigned long>(setWeight));
    out += ",\"weight\":";
    out += String(static_cast<long>(currentWeight));
    out += ",\"currentMenu\":";
    out += String(static_cast<unsigned>(currentMenuNumber()));
    out += ",\"enterMenu\":";
    out += (enterMenu ? "true" : "false");
    out += ",\"savedScaleName\":";
    appendJsonQuoted(out, savedName);
    out += ",\"savedScaleMac\":";
    appendJsonQuoted(out, savedMac);
    out += ",\"seenScaleName\":";
    appendJsonQuoted(out, connectedName);
    out += ",\"seenScaleMac\":";
    appendJsonQuoted(out, connectedMac);
    out += '}';
}

void appendMenuSnapshotJson(String& out, uint8_t menuNum) {
    if(menuNum == 1) {
        syncGaugeMenuValue();
    }
    if(menuNum == 5) {
        Menu5[Menu5Items::WIRELESS_RADIO_MODE].value = Menu1[Menu1Items::RADIOMODE].value;
    }
    const menuEntry* menu = menuByNumber(menuNum);
    if(menu == nullptr) {
        return;
    }

    const uint8_t count = menuCountByNumber(menuNum);
    const int8_t* selectedPtr = menuSelectionByNumber(menuNum);
    const uint8_t activeMenu = currentMenuNumber();

    out += "{\"menu\":";
    out += String(static_cast<unsigned>(menuNum));
    out += ",\"label\":";
    appendJsonQuoted(out, menuLabel(menuNum));
    out += ",\"active\":";
    out += (activeMenu == menuNum ? "true" : "false");
    out += ",\"selected\":";
    out += String(selectedPtr != nullptr ? static_cast<int>(*selectedPtr) : -1);
    out += ",\"items\":[";

    bool firstItem = true;
    for(uint8_t idx = 0; idx < count; ++idx) {
        if(!motor_menu_item_visible(menuNum, idx)) {
            continue;
        }
        if(!firstItem) out += ',';
        firstItem = false;
        const menuEntry& entry = menu[idx];
        const char* kind = apiMenuItemKind(menuNum, idx);
        out += '{';
        out += "\"index\":";
        out += String(static_cast<unsigned>(idx));
        out += ",\"name\":";
        appendJsonQuoted(out, entry.name);
        out += ",\"kind\":";
        appendJsonQuoted(out, kind);
        out += ",\"writable\":";
        out += (apiMenuItemWritable(menuNum, idx) ? "true" : "false");
        out += ",\"value\":";
        out += String(entry.value);
        out += ",\"min\":";
        out += String(entry.minValue);
        out += ",\"max\":";
        out += String(entry.maxValue);
        out += ",\"scalar\":";
        out += String(entry.scalar);
        out += ",\"unit\":";
        appendJsonQuoted(out, entry.unit);
        out += ",\"selected\":";
        out += (activeMenu == menuNum && selectedPtr != nullptr && *selectedPtr == static_cast<int8_t>(idx)) ? "true" : "false";

        const uint8_t targetMenu = apiTargetMenu(menuNum, idx);
        if(targetMenu != 0) {
            out += ",\"targetMenu\":";
            out += String(static_cast<unsigned>(targetMenu));
        }

        const char* action = apiMenuItemAction(menuNum, idx);
        if(action != nullptr) {
            out += ",\"action\":";
            appendJsonQuoted(out, action);
        }

        const uint32_t baud = (menuNum == 4) ? baudForJmcMenuIndex(idx) : 0;
        if(baud != 0) {
            out += ",\"baud\":";
            out += String(static_cast<unsigned long>(baud));
            out += ",\"activeBaud\":";
            const bool activeBaud =
                motor_get_drive_type() == DRIVE_TYPE_JMC &&
                commStatus == COMM_CONNECTED &&
                comm_get_jmc_baud() == baud;
            out += (activeBaud ? "true" : "false");
        }

        out += '}';
    }

    out += "]}";
}

void printHelp() {
    Serial.println("[diag] commands: ?=status, h=help, v=toggle stream, g=force IDLE_GBW, i=force IDLE, a=redetect, j=force JMC, r=force RT, k=force KRS, x=force XB130, s=force SEW, p=probe, l=dump log");
    Serial.println("[diag] jmc: jbaud <9600|19200|38400|57600|115200> (write drive profile and store same baud for next boot; reboot required)");
    Serial.println("[diag] config: jb <9600|19200|38400|57600|115200> (set preferred JMC boot baud, takes effect on redetect/reboot)");
    Serial.println("[diag] rs485: rs485 <onboard|ex_i2c> [save] (force active RS-485 port for raw tests/probe)");
    Serial.println("[diag] gpio: gpio ui | gpio ui float (raw front-panel button GPIO levels / GPIO42 bias test)");
    Serial.println("[diag] raw: mr <j|r> <slave> <reg> <count> | mw <j|r> <slave> <reg> <value>");
    Serial.println("[diag] raw-uart: mru <baud> <8N1|8N2|8E1|8E2|8O1|8O2> <slave> <reg> <count> | mwu <baud> <mode> <slave> <reg> <value>");
    Serial.println("[diag] dbg: dbg status | dbg wifi <on|off|reset|scan> | dbg espnow <on|off|coexist> | dbg ws <on|off> | dbg all off");
    Serial.println("[diag] api: api device | api status | api env | api wifi | api wifi_save <ssid> <password> | api wifi_forget");
    Serial.println("[diag] api: api ota boot <next|running|app0|app1> [reboot] | api ota apply_ready [reboot] | api ota invalidate_inactive | api ota check [url|latest] | api ota download_if_available [url|latest] | api ota download <url> [boot|reboot] | api ota manifest <url|latest> [boot|reboot] | api reboot");
    Serial.println("[diag] api: api snapshot | api set <menu> <item> <value> | api setrpm <rpm> | api setweight <mg>");
    Serial.println("[diag] api: api action <start|stop|idle|idle_gbw|redetect|calibrate|network_info|about|drive auto|drive jmc|drive rt|drive krs|drive xb130|drive sew|drive tse|jmc_baud <baud>|reset_network|reset_defaults confirm|ota_check [latest]|ota_download_if_available [latest]|ota_manifest [latest]|ota_apply_ready_reboot>");
}

void printStatus() {
    String connectedName;
    String connectedMac;
    String savedName;
    String savedMac;

    if(scaleMutex != NULL && xSemaphoreTake(scaleMutex, pdMS_TO_TICKS(25))) {
        connectedName = scale_name;
        connectedMac = scale_mac;
        savedName = scale_connect_name;
        savedMac = scale_connect_mac;
        xSemaphoreGive(scaleMutex);
    }

    Serial.printf(
        "[diag] drive=%s uart=%s jmc_pref=%lu state=%s comm=%s motor=%s err=%u setRPM=%d curRPM=%d torque=%d gap=%ldum gap_live=%s setWeight=%lu weight=%ld scale=%s saved_name=\"%s\" saved_mac=\"%s\" seen_name=\"%s\" seen_mac=\"%s\"\n",
        motor_get_type_label(),
        comm_get_profile_label(),
        static_cast<unsigned long>(motor_get_preferred_jmc_baud()),
        stateText(state),
        commText(commStatus),
        motorText(currentStatus),
        static_cast<unsigned>(error),
        motor_get_effective_set_rpm(),
        motor_currentRPM,
        motor_currentTorque,
        static_cast<long>(burr_gap_display_lcd_um()),
        burr_gap_live() ? "yes" : "no",
        static_cast<unsigned long>(setWeight),
        static_cast<long>(currentWeight),
        scaleText(scaleStatus),
        savedName.c_str(),
        savedMac.c_str(),
        connectedName.c_str(),
        connectedMac.c_str()
    );
}

void forceIdleMode(States nextState) {
    enterMenu = false;
    menu1Selected = EXITMENU;
    menu2Selected = RETURN_FROM_PURGE;
    menu3Selected = RETURN_FROM_GBW;
    menu4Selected = RETURN_FROM_JMC_BAUD;
    menu5Selected = RETURN_FROM_WIRELESS;
    motor_setRPM = 0;
    state = nextState;
    controller_note_activity();
}

void printProbeReport() {
    const MotorProbeReport report = motor_run_probe_report();
    Serial.printf(
        "[diag] probe jmc_6041=%s jmc_1010=%s jmc_6040w=%s jmc_baud=%lu rt_1300=%s krs20=%s xb130_20=%s sew_3pd=%s sew_addr=%u tse=%s active_drive=%s active_uart=%s rs485_port=%s\n",
        report.jmc_status_word ? "ok" : "no",
        report.jmc_magic_word ? "ok" : "no",
        report.jmc_control_word ? "ok" : "no",
        static_cast<unsigned long>(report.jmc_baud),
        report.rt_status_block ? "ok" : "no",
        report.krs_130st_m07725_motor_code ? "ok" : "no",
        report.krs_xb130aea20025_shl_motor_code ? "ok" : "no",
        report.sew_movilink_3pd ? "ok" : "no",
        static_cast<unsigned>(report.sew_movilink_address),
        report.tse_signature ? "ok" : "no",
        motor_get_type_label(),
        comm_get_profile_label(),
        comm_get_rs485_port_label(comm_get_rs485_port())
    );
}

void emitHistoryLine(const char* line, bool toSerial, bool toBle) {
    if(toSerial) {
        Serial.println(line);
    }
    if(toBle) {
        ble_console_send_line(line);
    }
}

void printDebugStatus() {
    Serial.print("{\"type\":\"dbg\",\"wifi\":");
    Serial.print(wifi_ui_status_json());
    Serial.print(",\"espnowActive\":");
    Serial.print(espnow_control_is_active() ? "true" : "false");
    Serial.print(",\"espnowChannel\":");
    Serial.print(static_cast<unsigned>(espnow_control_channel()));
    Serial.print(",\"espnowStatus\":");
    jsonPrintQuoted(espnow_control_status_text());
    Serial.print(",\"wsActive\":");
    Serial.print(remote_ws_is_active() ? "true" : "false");
    Serial.print(",\"wifiMode\":");
    Serial.print(static_cast<unsigned>(WiFi.getMode()));
    Serial.print(",\"wifiStatusCode\":");
    Serial.print(static_cast<int>(WiFi.status()));
    Serial.println("}");
}

void handleDebugCommand(char* savePtr, const char* originalLine) {
    char* target = strtok_r(nullptr, " \t", &savePtr);
    if(target == nullptr || strcmp(target, "status") == 0) {
        printDebugStatus();
        return;
    }

    char* action = strtok_r(nullptr, " \t", &savePtr);
    if(strcmp(target, "wifi") == 0) {
        if(action == nullptr) {
            apiPrintAck("dbg", false, "usage: dbg wifi <on|off|reset|scan>");
            return;
        }
        if(strcmp(action, "on") == 0) {
            apiPrintAck("dbg", wifi_ui_start(), "wifi on");
            return;
        }
        if(strcmp(action, "off") == 0) {
            const bool espWasActive = espnow_control_is_active();
            if(espWasActive) {
                espnow_control_stop();
            }
            wifi_ui_stop();
            const bool ok = !espWasActive || espnow_control_start(false);
            apiPrintAck("dbg", ok, ok ? "wifi off" : "wifi off; esp-now restart failed");
            return;
        }
        if(strcmp(action, "reset") == 0) {
            wifi_ui_reset_network_settings();
            apiPrintAck("dbg", true, "wifi reset");
            return;
        }
        if(strcmp(action, "scan") == 0) {
            Serial.println(wifi_ui_scan_json(true));
            return;
        }
        apiPrintAck("dbg", false, "usage: dbg wifi <on|off|reset|scan>");
        return;
    }

    if(strcmp(target, "espnow") == 0) {
        if(action == nullptr) {
            apiPrintAck("dbg", false, "usage: dbg espnow <on|off|coexist>");
            return;
        }
        if(strcmp(action, "off") == 0) {
            const bool webWasActive = wifi_ui_is_active();
            espnow_control_stop();
            const bool ok = !webWasActive || wifi_ui_start();
            apiPrintAck("dbg", ok, ok ? "esp-now off" : "esp-now off; wifi restart failed");
            return;
        }
        if(strcmp(action, "on") == 0) {
            const bool webWasActive = wifi_ui_is_active();
            if(webWasActive) {
                wifi_ui_stop();
            }
            espnow_control_stop();
            const bool ok = espnow_control_start(false);
            apiPrintAck("dbg", ok, ok ? "esp-now on" : "esp-now start failed");
            return;
        }
        if(strcmp(action, "coexist") == 0) {
            espnow_control_stop();
            const bool ok = wifi_ui_start() && espnow_control_start(true);
            apiPrintAck("dbg", ok, ok ? "esp-now coexist" : "coexist start failed");
            return;
        }
        apiPrintAck("dbg", false, "usage: dbg espnow <on|off|coexist>");
        return;
    }

    if(strcmp(target, "ws") == 0) {
        if(action == nullptr) {
            apiPrintAck("dbg", false, "usage: dbg ws <on|off>");
            return;
        }
        if(strcmp(action, "on") == 0) {
            apiPrintAck("dbg", remote_ws_start(), "ws on");
            return;
        }
        if(strcmp(action, "off") == 0) {
            remote_ws_stop();
            apiPrintAck("dbg", true, "ws off");
            return;
        }
        apiPrintAck("dbg", false, "usage: dbg ws <on|off>");
        return;
    }

    if(strcmp(target, "all") == 0 && action != nullptr && strcmp(action, "off") == 0) {
        remote_ws_stop();
        wifi_ui_stop();
        espnow_control_stop();
        apiPrintAck("dbg", true, "all off");
        return;
    }

    apiPrintAck("dbg", false, originalLine != nullptr ? "usage: dbg status|wifi|espnow|ws|all" : "usage");
}

void dumpErrorHistory(bool toSerial, bool toBle) {
    char line[128];
    const uint16_t count = error_history_count();

    snprintf(line, sizeof(line), "__BEGIN__ count=%u", static_cast<unsigned>(count));
    emitHistoryLine(line, toSerial, toBle);

    if(count == 0) {
        emitHistoryLine("no logged errors", toSerial, toBle);
        emitHistoryLine("__END__", toSerial, toBle);
        return;
    }

    for(uint16_t i = 0; i < count; ++i) {
        if((i % 8U) == 0U) {
            delay(1);
        }

        ErrorHistoryEntry entry;
        if(!error_history_get(i, &entry)) {
            continue;
        }

        switch(entry.code) {
            case ERROR_HISTORY_CODE_SYSTEM_ERROR: {
                const uint8_t currentError = static_cast<uint8_t>((entry.value >> 16) & 0xFFU);
                const uint16_t fault = static_cast<uint16_t>(entry.value & 0xFFFFU);
                if(currentError == 3) {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s SYS err=%u:%s flt=0x%02X st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             static_cast<unsigned>(currentError),
                             errorText(currentError),
                             static_cast<unsigned>(fault),
                             shortStateTextRaw(entry.state));
                } else {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s SYS err=%u:%s st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             static_cast<unsigned>(currentError),
                             errorText(currentError),
                             shortStateTextRaw(entry.state));
                }
                break;
            }
            case ERROR_HISTORY_CODE_MOTOR_COMM_STATUS:
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s MOT comm=%s st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         shortCommTextRaw(static_cast<uint8_t>(entry.value)),
                         shortStateTextRaw(entry.state));
                break;
            case ERROR_HISTORY_CODE_MOTOR_STATUS:
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s MOT stat=%s st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         shortMotorTextRaw(static_cast<uint8_t>(entry.value)),
                         shortStateTextRaw(entry.state));
                break;
            case ERROR_HISTORY_CODE_MOTOR_PROBE: {
                const uint8_t detectedType = static_cast<uint8_t>((entry.value >> 8) & 0xFFU);
                const char* jmcBaudLabel = "-";
                if((entry.value & (1UL << 16)) != 0) jmcBaudLabel = "9600";
                if((entry.value & (1UL << 17)) != 0) jmcBaudLabel = "19200";
                if((entry.value & (1UL << 18)) != 0) jmcBaudLabel = "38400";
                if((entry.value & (1UL << 19)) != 0) jmcBaudLabel = "57600";
                if((entry.value & (1UL << 20)) != 0) jmcBaudLabel = "115200";
                const char* detectedLabel = "AUTO";
                if(detectedType == DRIVE_TYPE_JMC) detectedLabel = "JMC";
                if(detectedType == DRIVE_TYPE_RT) detectedLabel = "RT";
                if(detectedType == DRIVE_TYPE_KRS_130ST_M07725) detectedLabel = "KRS";
                if(detectedType == DRIVE_TYPE_KRS_XB130AEA20025_SHL) detectedLabel = "XB130";
                if(detectedType == DRIVE_TYPE_SEW_MOVILINK) detectedLabel = "SEW";
                if(detectedType == DRIVE_TYPE_TSE) detectedLabel = "TSE";
                const uint8_t sewAddress = static_cast<uint8_t>((entry.value >> 24) & 0xFFU);
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s PROBE j6041=%u j1010=%u j6040w=%u jbaud=%s rt1300=%u krs20=%u xb20=%u sew3pd=%u sewaddr=%u pick=%s st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         static_cast<unsigned>((entry.value >> 0) & 0x1U),
                         static_cast<unsigned>((entry.value >> 1) & 0x1U),
                         static_cast<unsigned>((entry.value >> 2) & 0x1U),
                         jmcBaudLabel,
                         static_cast<unsigned>((entry.value >> 3) & 0x1U),
                         static_cast<unsigned>((entry.value >> 4) & 0x1U),
                         static_cast<unsigned>((entry.value >> 5) & 0x1U),
                         static_cast<unsigned>((entry.value >> 6) & 0x1U),
                         static_cast<unsigned>(sewAddress),
                         detectedLabel,
                         shortStateTextRaw(entry.state));
                break;
            }
            case ERROR_HISTORY_CODE_MOTOR_RPM_TEST: {
                const uint8_t subtype = static_cast<uint8_t>((entry.value >> 24) & 0xFFU);
                const uint8_t source = static_cast<uint8_t>((entry.value >> 16) & 0xFFU);
                const int16_t rpmValue = static_cast<int16_t>(entry.value & 0xFFFFU);
                if(subtype == ERROR_HISTORY_MOTOR_RPM_SAMPLE) {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s MOT rpm src=%s val=%d st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             rpmSourceText(source),
                             static_cast<int>(rpmValue),
                             shortStateTextRaw(entry.state));
                } else if(subtype == ERROR_HISTORY_MOTOR_RPM_ZERO) {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s MOT rpm src=%s zero set=%d st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             rpmSourceText(source),
                             static_cast<int>(rpmValue),
                             shortStateTextRaw(entry.state));
                } else {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s MOT rpm src=%s raw=%lu st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             rpmSourceText(source),
                             static_cast<unsigned long>(entry.value),
                             shortStateTextRaw(entry.state));
                }
                break;
            }
            case ERROR_HISTORY_CODE_MOTOR_JMC_DIAG: {
                const uint8_t subtype = static_cast<uint8_t>((entry.value >> 24) & 0xFFU);
                const uint16_t diagValue = static_cast<uint16_t>(entry.value & 0xFFFFU);
                if(subtype == ERROR_HISTORY_MOTOR_DIAG_MODE_RESPONSE) {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s MOT jmc mode=0x%02X st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             static_cast<unsigned>(diagValue & 0xFFU),
                             shortStateTextRaw(entry.state));
                } else if(subtype == ERROR_HISTORY_MOTOR_DIAG_DRIVE_ERROR) {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s MOT jmc err=0x%04X st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             static_cast<unsigned>(diagValue),
                             shortStateTextRaw(entry.state));
                } else {
                    snprintf(line,
                             sizeof(line),
                             "#%lu t=%lums %s MOT jmc raw=0x%08lX st=%s",
                             static_cast<unsigned long>(entry.sequence),
                             static_cast<unsigned long>(entry.uptime_ms),
                             levelText(entry.level),
                             static_cast<unsigned long>(entry.value),
                             shortStateTextRaw(entry.state));
                }
                break;
            }
            case ERROR_HISTORY_CODE_MOTOR_JMC_BAUD: {
                const uint8_t subtype = static_cast<uint8_t>((entry.value >> 24) & 0xFFU);
                const uint32_t baud = entry.value & 0x00FFFFFFUL;
                const char* action = "raw";
                if(subtype == ERROR_HISTORY_MOTOR_JMC_BAUD_DETECTED) action = "detect";
                if(subtype == ERROR_HISTORY_MOTOR_JMC_BAUD_PREFERRED) action = "prefer";
                if(subtype == ERROR_HISTORY_MOTOR_JMC_BAUD_READ_BEFORE) action = "read_before";
                if(subtype == ERROR_HISTORY_MOTOR_JMC_BAUD_READ_AFTER) action = "read_after";
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s MOT jmc baud=%lu action=%s st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         static_cast<unsigned long>(baud),
                         action,
                         shortStateTextRaw(entry.state));
                break;
            }
            case ERROR_HISTORY_CODE_MOTOR_JMC_BAUD_TRIAL: {
                const uint8_t subtype = static_cast<uint8_t>((entry.value >> 24) & 0xFFU);
                const uint32_t baud = entry.value & 0x00FFFFFFUL;
                const char* result = "raw";
                switch(subtype) {
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_STARTED: result = "start"; break;
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_OK: result = "ok"; break;
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_ALREADY: result = "already"; break;
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_NO_JMC: result = "no_jmc"; break;
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_WRITE_FAIL: result = "write_fail"; break;
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_VERIFY_FAIL: result = "verify_fail"; break;
                    case ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_LOST: result = "lost"; break;
                }
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s MOT jmc switch=%s baud=%lu st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         result,
                         static_cast<unsigned long>(baud),
                         shortStateTextRaw(entry.state));
                break;
            }
            case ERROR_HISTORY_CODE_SCALE_STATUS:
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s SCALE stat=%s st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         shortScaleTextRaw(static_cast<uint8_t>(entry.value)),
                         shortStateTextRaw(entry.state));
                break;
            default:
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s src=%u code=%u val=%lu st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         static_cast<unsigned>(entry.source),
                         static_cast<unsigned>(entry.code),
                         static_cast<unsigned long>(entry.value),
                         shortStateTextRaw(entry.state));
                break;
        }

        emitHistoryLine(line, toSerial, toBle);
    }

    emitHistoryLine("__END__", toSerial, toBle);
}

void dumpBleErrorHistory() {
    dumpErrorHistory(false, true);
}

bool parseUnsignedToken(const char* token, uint32_t* value) {
    if(token == nullptr || value == nullptr) {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = strtoul(token, &end, 0);
    if(end == token || (end != nullptr && *end != '\0')) {
        return false;
    }

    *value = parsed;
    return true;
}

bool parseProfileToken(const char* token, CommProfile* profile) {
    if(token == nullptr || profile == nullptr) {
        return false;
    }

    if(strcmp(token, "tse") == 0 || strcmp(token, "TSE") == 0) {
        *profile = COMM_PROFILE_TSE;
        return true;
    }

    if(strcmp(token, "j") == 0 || strcmp(token, "J") == 0 || strcmp(token, "jmc") == 0 || strcmp(token, "JMC") == 0) {
        *profile = COMM_PROFILE_JMC;
        return true;
    }

    if(strcmp(token, "r") == 0 || strcmp(token, "R") == 0 || strcmp(token, "rt") == 0 || strcmp(token, "RT") == 0) {
        *profile = COMM_PROFILE_RT;
        return true;
    }

    if(strcmp(token, "k") == 0 || strcmp(token, "K") == 0 || strcmp(token, "krs") == 0 || strcmp(token, "KRS") == 0) {
        *profile = COMM_PROFILE_KRS;
        return true;
    }

    return false;
}

bool parseRs485PortToken(const char* token, CommRs485Port* port) {
    if(token == nullptr || port == nullptr) {
        return false;
    }

    if(strcmp(token, "onboard") == 0 ||
       strcmp(token, "o") == 0 ||
       strcmp(token, "internal") == 0) {
        *port = COMM_RS485_PORT_ONBOARD;
        return true;
    }

    if(strcmp(token, "ex_i2c") == 0 ||
       strcmp(token, "ex") == 0 ||
       strcmp(token, "ext") == 0 ||
       strcmp(token, "external") == 0) {
        *port = COMM_RS485_PORT_EX_I2C_WORKAROUND;
        return true;
    }

    return false;
}

bool parseSerialConfigToken(const char* token, uint32_t* serialConfig) {
    if(token == nullptr || serialConfig == nullptr) {
        return false;
    }

    if(strcmp(token, "8N1") == 0) { *serialConfig = SERIAL_8N1; return true; }
    if(strcmp(token, "8N2") == 0) { *serialConfig = SERIAL_8N2; return true; }
    if(strcmp(token, "8E1") == 0) { *serialConfig = SERIAL_8E1; return true; }
    if(strcmp(token, "8E2") == 0) { *serialConfig = SERIAL_8E2; return true; }
    if(strcmp(token, "8O1") == 0) { *serialConfig = SERIAL_8O1; return true; }
    if(strcmp(token, "8O2") == 0) { *serialConfig = SERIAL_8O2; return true; }
    return false;
}

void printFrameHex(const char* label, const uint8_t* data, uint8_t len) {
    Serial.print(label);
    if(len == 0) {
        Serial.println("<none>");
        return;
    }

    for(uint8_t i = 0; i < len; ++i) {
        Serial.printf("%02X", data[i]);
        if(i + 1 < len) {
            Serial.print(" ");
        }
    }
    Serial.println();
}

void printRawTransaction(const char* opLabel,
                         CommProfile profile,
                         uint8_t slave,
                         uint16_t reg,
                         uint16_t arg,
                         const char* argLabel,
                         const CommDebugTransaction& transaction) {
    Serial.printf(
        "[diag] raw %s profile=%s slave=%u reg=0x%04X %s=%u ok=%s timeout=%s crc=%s parsed=0x%04X exc=%u req_len=%u rsp_len=%u\n",
        opLabel,
        profileText(profile),
        static_cast<unsigned>(slave),
        static_cast<unsigned>(reg),
        argLabel,
        static_cast<unsigned>(arg),
        transaction.success ? "yes" : "no",
        transaction.timed_out ? "yes" : "no",
        transaction.response_crc_ok ? "ok" : "bad",
        static_cast<unsigned>(transaction.parsed_reg < 0 ? 0xFFFFU : static_cast<uint16_t>(transaction.parsed_reg)),
        static_cast<unsigned>(transaction.exception_code),
        static_cast<unsigned>(transaction.request_len),
        static_cast<unsigned>(transaction.response_len)
    );
    printFrameHex("[diag] tx ", transaction.request, transaction.request_len);
    printFrameHex("[diag] rx ", transaction.response, transaction.response_len);
}

void printRawTransactionUart(const char* opLabel,
                             uint32_t baudRate,
                             const char* uartMode,
                             uint8_t slave,
                             uint16_t reg,
                             uint16_t arg,
                             const char* argLabel,
                             const CommDebugTransaction& transaction) {
    Serial.printf(
        "[diag] raw %s uart=%lu/%s slave=%u reg=0x%04X %s=%u ok=%s timeout=%s crc=%s parsed=0x%04X exc=%u req_len=%u rsp_len=%u\n",
        opLabel,
        static_cast<unsigned long>(baudRate),
        uartMode,
        static_cast<unsigned>(slave),
        static_cast<unsigned>(reg),
        argLabel,
        static_cast<unsigned>(arg),
        transaction.success ? "yes" : "no",
        transaction.timed_out ? "yes" : "no",
        transaction.response_crc_ok ? "ok" : "bad",
        static_cast<unsigned>(transaction.parsed_reg < 0 ? 0xFFFFU : static_cast<uint16_t>(transaction.parsed_reg)),
        static_cast<unsigned>(transaction.exception_code),
        static_cast<unsigned>(transaction.request_len),
        static_cast<unsigned>(transaction.response_len)
    );
    printFrameHex("[diag] tx ", transaction.request, transaction.request_len);
    printFrameHex("[diag] rx ", transaction.response, transaction.response_len);
}

void handleLineCommand(char* line) {
    if(line == nullptr) {
        return;
    }

    while(*line == ' ' || *line == '\t') {
        ++line;
    }
    if(*line == '\0') {
        return;
    }

    char* savePtr = nullptr;
    char* cmd = strtok_r(line, " \t", &savePtr);
    if(cmd == nullptr) {
        return;
    }

    if(strcmp(cmd, "api") == 0 || strcmp(cmd, "API") == 0) {
        char* subcmd = strtok_r(nullptr, " \t", &savePtr);
        if(subcmd == nullptr) {
            apiPrintAck("api", false, "missing subcommand");
            return;
        }

        if(strcmp(subcmd, "snapshot") == 0) {
            apiPrintSnapshot();
            return;
        }

        if(strcmp(subcmd, "device") == 0 || strcmp(subcmd, "identity") == 0) {
            apiPrintDevice();
            return;
        }

        if(strcmp(subcmd, "status") == 0) {
            apiPrintStatus();
            return;
        }

        if(strcmp(subcmd, "gauge_probe") == 0) {
            uint32_t duration = kGaugeProbeDefaultMs;
            uint8_t clkPin = 0;
            uint8_t dataPin = 0;
            bool usingDefaultMapping = defaultGaugeProbeMapping(&clkPin, &dataPin, nullptr);

            char* firstArg = strtok_r(nullptr, " \t", &savePtr);
            if(firstArg != nullptr) {
                if(strcmp(firstArg, "auto") == 0 || strcmp(firstArg, "both") == 0) {
                    uint32_t requestedDuration = duration;
                    char* durationArg = strtok_r(nullptr, " \t", &savePtr);
                    if(durationArg != nullptr && !parseUnsignedToken(durationArg, &requestedDuration)) {
                        apiPrintAck("gauge_probe", false, "usage: api gauge_probe [auto [duration_ms] | duration_ms [clk_gpio data_gpio]]");
                        return;
                    }
                    if(requestedDuration < kGaugeProbeMinMs) requestedDuration = kGaugeProbeMinMs;
                    if(requestedDuration > kGaugeProbeMaxMs) requestedDuration = kGaugeProbeMaxMs;
                    apiPrintGaugeProbeAuto(static_cast<uint16_t>(requestedDuration));
                    return;
                }

                if(!parseUnsignedToken(firstArg, &duration)) {
                    apiPrintAck("gauge_probe", false, "usage: api gauge_probe [auto [duration_ms] | duration_ms [clk_gpio data_gpio]]");
                    return;
                }

                char* clkArg = strtok_r(nullptr, " \t", &savePtr);
                char* dataArg = strtok_r(nullptr, " \t", &savePtr);
                if(clkArg != nullptr || dataArg != nullptr) {
                    uint32_t parsedClk = 0;
                    uint32_t parsedData = 0;
                    if(clkArg == nullptr || dataArg == nullptr ||
                       !parseUnsignedToken(clkArg, &parsedClk) ||
                       !parseUnsignedToken(dataArg, &parsedData) ||
                       !gpioProbePinValid(parsedClk) ||
                       !gpioProbePinValid(parsedData)) {
                        apiPrintAck("gauge_probe", false, "usage: api gauge_probe [auto [duration_ms] | duration_ms [clk_gpio data_gpio]]");
                        return;
                    }
                    clkPin = static_cast<uint8_t>(parsedClk);
                    dataPin = static_cast<uint8_t>(parsedData);
                    usingDefaultMapping = true;
                }
            }

            if(!usingDefaultMapping) {
                apiPrintAck("gauge_probe", false, "no burr gap GPIO mapping for this controller");
                return;
            }
            if(duration < kGaugeProbeMinMs) duration = kGaugeProbeMinMs;
            if(duration > kGaugeProbeMaxMs) duration = kGaugeProbeMaxMs;
            if(!gpioProbePinValid(clkPin) || !gpioProbePinValid(dataPin)) {
                apiPrintAck("gauge_probe", false, "invalid GPIO");
                return;
            }
            apiPrintGaugeProbe(static_cast<uint16_t>(duration), clkPin, dataPin);
            return;
        }

        if(strcmp(subcmd, "env") == 0 || strcmp(subcmd, "environment") == 0) {
            Serial.println(controller_env_build_frame());
            return;
        }

        if(strcmp(subcmd, "wifi") == 0) {
            Serial.println(wifi_ui_status_json());
            return;
        }

        if(strcmp(subcmd, "wifi_scan") == 0) {
            const char* arg = strtok_r(nullptr, " \t", &savePtr);
            Serial.println(wifi_ui_scan_json(arg == nullptr || strcmp(arg, "force") == 0 || strcmp(arg, "1") == 0));
            return;
        }

        if(strcmp(subcmd, "wifi_save") == 0) {
            char* ssid = strtok_r(nullptr, " \t", &savePtr);
            char* password = strtok_r(nullptr, " \t", &savePtr);
            if(ssid == nullptr || ssid[0] == '\0') {
                apiPrintAck("wifi_save", false, "usage: api wifi_save <ssid> <password>");
                return;
            }
            const bool ok = wifi_ui_save_network_settings(String(ssid), password != nullptr ? String(password) : String(""));
            apiPrintAck("wifi_save", ok, ok ? "saved" : "cleared");
            return;
        }

        if(strcmp(subcmd, "wifi_forget") == 0) {
            wifi_ui_reset_network_settings();
            apiPrintAck("wifi_forget", true, "forgotten");
            return;
        }

        if(strcmp(subcmd, "ota") == 0) {
            char* action = strtok_r(nullptr, " \t", &savePtr);
            if(action == nullptr) {
                apiPrintAck("ota", false, "usage: api ota boot <next|running|app0|app1> [reboot] | api ota apply_ready [reboot] | api ota invalidate_inactive | api ota check [url|latest] | api ota download_if_available [url|latest] | api ota download <url> [boot|reboot] | api ota manifest <url|latest> [boot|reboot]");
                return;
            }
            if(strcmp(action, "boot") == 0) {
                const char* target = strtok_r(nullptr, " \t", &savePtr);
                const char* rebootToken = strtok_r(nullptr, " \t", &savePtr);
                const char* message = nullptr;
                const bool ok = apiSetOtaBootPartition(target, &message);
                apiPrintAck("ota", ok, message);
                if(ok && rebootToken != nullptr && strcmp(rebootToken, "reboot") == 0) {
                    delay(150);
                    Serial.flush();
                    ESP.restart();
                }
                return;
            }
            if(strcmp(action, "apply_ready") == 0) {
                const char* rebootToken = strtok_r(nullptr, " \t", &savePtr);
                String message;
                const bool ok = controller_ota_mark_ready_bootable(&message);
                apiPrintAck("ota", ok, message.c_str());
                if(ok && rebootToken != nullptr && strcmp(rebootToken, "reboot") == 0) {
                    delay(150);
                    Serial.flush();
                    ESP.restart();
                }
                return;
            }
            if(strcmp(action, "invalidate_inactive") == 0) {
                String message;
                const bool ok = controller_ota_invalidate_inactive_slot(&message);
                apiPrintAck("ota", ok, message.c_str());
                return;
            }
            if(strcmp(action, "check") == 0) {
                const char* url = strtok_r(nullptr, " \t", &savePtr);
                String message;
                const bool ok = apiCheckOtaManifest(url != nullptr ? url : "latest", &message);
                apiPrintAck("ota", ok, message.c_str());
                return;
            }
            if(strcmp(action, "download_if_available") == 0) {
                const char* url = strtok_r(nullptr, " \t", &savePtr);
                String message;
                const bool ok = apiDownloadOtaIfAvailable(url != nullptr ? url : "latest", &message);
                apiPrintAck("ota", ok, message.c_str());
                return;
            }
            if(strcmp(action, "download") == 0) {
                const char* url = strtok_r(nullptr, " \t", &savePtr);
                const char* bootToken = strtok_r(nullptr, " \t", &savePtr);
                String message;
                const bool ok = apiDownloadOta(url, &message);
                apiPrintAck("ota", ok, message.c_str());
                if(ok && bootToken != nullptr &&
                   (strcmp(bootToken, "boot") == 0 || strcmp(bootToken, "reboot") == 0)) {
                    const char* bootMessage = nullptr;
                    const bool bootOk = apiSetOtaBootPartition("next", &bootMessage);
                    apiPrintAck("ota", bootOk, bootMessage);
                    if(bootOk && strcmp(bootToken, "reboot") == 0) {
                        delay(150);
                        Serial.flush();
                        ESP.restart();
                    }
                }
                return;
            }
            if(strcmp(action, "manifest") == 0) {
                const char* url = strtok_r(nullptr, " \t", &savePtr);
                const char* bootToken = strtok_r(nullptr, " \t", &savePtr);
                String message;
                const bool ok = apiDownloadOtaManifest(url, &message);
                apiPrintAck("ota", ok, message.c_str());
                if(ok && bootToken != nullptr &&
                   (strcmp(bootToken, "boot") == 0 || strcmp(bootToken, "reboot") == 0)) {
                    const char* bootMessage = nullptr;
                    const bool bootOk = apiSetOtaBootPartition("next", &bootMessage);
                    apiPrintAck("ota", bootOk, bootMessage);
                    if(bootOk && strcmp(bootToken, "reboot") == 0) {
                        delay(150);
                        Serial.flush();
                        ESP.restart();
                    }
                }
                return;
            }
            if(strcmp(action, "reboot") == 0) {
                apiPrintAck("ota", true, "rebooting");
                delay(150);
                Serial.flush();
                ESP.restart();
                return;
            }
            apiPrintAck("ota", false, "usage: api ota boot <next|running|app0|app1> [reboot] | api ota apply_ready [reboot] | api ota invalidate_inactive | api ota check [url|latest] | api ota download_if_available [url|latest] | api ota download <url> [boot|reboot] | api ota manifest <url|latest> [boot|reboot]");
            return;
        }

        if(strcmp(subcmd, "reboot") == 0) {
            apiPrintAck("reboot", true, "rebooting");
            delay(150);
            Serial.flush();
            ESP.restart();
            return;
        }

        if(strcmp(subcmd, "set") == 0) {
            uint32_t menuNum = 0;
            uint32_t itemIdx = 0;
            uint32_t value = 0;
            if(!parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &menuNum) ||
               !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &itemIdx) ||
               !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &value)) {
                apiPrintAck("set", false, "usage: api set <menu> <item> <value>");
                return;
            }

            const char* message = nullptr;
            const bool ok = apiSetMenuValue(static_cast<uint8_t>(menuNum),
                                            static_cast<uint8_t>(itemIdx),
                                            static_cast<int32_t>(value),
                                            &message);
            apiPrintAck("set", ok, message);
            return;
        }

        if(strcmp(subcmd, "setrpm") == 0) {
            uint32_t value = 0;
            if(!parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &value)) {
                apiPrintAck("setrpm", false, "usage: api setrpm <rpm>");
                return;
            }

            const char* message = nullptr;
            const bool ok = apiSetRpmValue(static_cast<int32_t>(value), &message);
            apiPrintAck("setrpm", ok, message);
            return;
        }

        if(strcmp(subcmd, "setweight") == 0) {
            uint32_t value = 0;
            if(!parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &value)) {
                apiPrintAck("setweight", false, "usage: api setweight <mg>");
                return;
            }

            const char* message = nullptr;
            const bool ok = apiSetWeightValue(static_cast<int32_t>(value), &message);
            apiPrintAck("setweight", ok, message);
            return;
        }

        if(strcmp(subcmd, "action") == 0) {
            char* action = strtok_r(nullptr, " \t", &savePtr);
            if(action == nullptr) {
                apiPrintAck("action", false, "missing action");
                return;
            }

            const char* message = nullptr;
            bool ok = false;

            if(strcmp(action, "start") == 0) {
                ok = apiActionStart(&message);
            } else if(strcmp(action, "stop") == 0) {
                ok = apiActionStop(&message);
            } else if(strcmp(action, "idle") == 0) {
                ok = apiActionIdle(IDLE, &message);
            } else if(strcmp(action, "idle_gbw") == 0) {
                ok = apiActionIdle(IDLE_GBW, &message);
            } else if(strcmp(action, "redetect") == 0) {
                ok = apiActionDrive("auto", &message);
            } else if(strcmp(action, "drive") == 0) {
                ok = apiActionDrive(strtok_r(nullptr, " \t", &savePtr), &message);
            } else if(strcmp(action, "calibrate") == 0) {
                ok = apiActionCalibrate(&message);
            } else if(strcmp(action, "jmc_baud") == 0) {
                uint32_t baud = 0;
                if(!parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &baud)) {
                    apiPrintAck("action", false, "usage: api action jmc_baud <baud>");
                    return;
                }
                if(!motor_jmc_tools_supported()) {
                    ok = false;
                    message = "JMC baud unavailable for active drive";
                } else {
                    ok = motor_run_jmc_baud_switch(baud);
                    message = motor_get_jmc_baud_trial_status_text();
                    disp_updateRequired = true;
                }
            } else if(strcmp(action, "reset_network") == 0) {
                ok = apiActionResetNetwork(&message);
            } else if(strcmp(action, "network_info") == 0) {
                ok = apiActionNetworkInfo(&message);
            } else if(strcmp(action, "about") == 0) {
                ok = apiActionAbout(&message);
            } else if(strcmp(action, "reset_defaults") == 0) {
                ok = apiActionResetDefaults(strtok_r(nullptr, " \t", &savePtr), &message);
            } else {
                apiPrintAck("action", false, "unknown action");
                return;
            }

            apiPrintAck("action", ok, message);
            return;
        }

        apiPrintAck("api", false, "unknown subcommand");
        return;
    }

    if(strcmp(cmd, "dbg") == 0 || strcmp(cmd, "DBG") == 0) {
        handleDebugCommand(savePtr, line);
        return;
    }

    if(strcmp(cmd, "gpio") == 0 || strcmp(cmd, "GPIO") == 0) {
        char* subcmd = strtok_r(nullptr, " \t", &savePtr);
        if(subcmd == nullptr || strcmp(subcmd, "ui") == 0 || strcmp(subcmd, "UI") == 0) {
            char* mode = strtok_r(nullptr, " \t", &savePtr);
            const ControllerBoardPins& pins = controller_board_pins();
            const bool hasStart = controller_board_has_start_button() &&
                                  controller_board_pin_valid(pins.startButton);
            const bool hasEncoder = controller_board_has_encoder_button() &&
                                    controller_board_pin_valid(pins.encoderButton);
            const int startRaw = hasStart ? digitalRead(pins.startButton) : -1;
            const int encoderRaw = hasEncoder ? digitalRead(pins.encoderButton) : -1;
            if(mode != nullptr && (strcmp(mode, "float") == 0 || strcmp(mode, "FLOAT") == 0)) {
                int floatingHigh = 0;
                int floatingLow = 0;
                int pulldownHigh = 0;
                int pulldownLow = 0;
                int restoredRaw = -1;
                if(hasEncoder) {
                    pinMode(pins.encoderButton, INPUT);
                    delayMicroseconds(1500);
                    for(uint8_t i = 0; i < 24; ++i) {
                        if(digitalRead(pins.encoderButton) == HIGH) {
                            ++floatingHigh;
                        } else {
                            ++floatingLow;
                        }
                        delayMicroseconds(100);
                    }
                    pinMode(pins.encoderButton, INPUT_PULLDOWN);
                    delayMicroseconds(1500);
                    for(uint8_t i = 0; i < 24; ++i) {
                        if(digitalRead(pins.encoderButton) == HIGH) {
                            ++pulldownHigh;
                        } else {
                            ++pulldownLow;
                        }
                        delayMicroseconds(100);
                    }
                    pinMode(pins.encoderButton, INPUT_PULLUP);
                    delayMicroseconds(500);
                    restoredRaw = digitalRead(pins.encoderButton);
                }
                Serial.printf("[diag] gpio ui float board=%s enc_pin=%d pullup_raw=%s floating_high=%d floating_low=%d pulldown_high=%d pulldown_low=%d restored_raw=%s\n",
                              controller_board_target_name(),
                              static_cast<int>(pins.encoderButton),
                              encoderRaw < 0 ? "NA" : (encoderRaw == HIGH ? "HIGH" : "LOW"),
                              floatingHigh,
                              floatingLow,
                              pulldownHigh,
                              pulldownLow,
                              restoredRaw < 0 ? "NA" : (restoredRaw == HIGH ? "HIGH" : "LOW"));
                return;
            }
            Serial.printf("[diag] gpio ui board=%s start_pin=%d start_raw=%s start_pressed_low=%s enc_pin=%d enc_raw=%s enc_pressed_low=%s\n",
                          controller_board_target_name(),
                          static_cast<int>(pins.startButton),
                          startRaw < 0 ? "NA" : (startRaw == HIGH ? "HIGH" : "LOW"),
                          startRaw == LOW ? "yes" : "no",
                          static_cast<int>(pins.encoderButton),
                          encoderRaw < 0 ? "NA" : (encoderRaw == HIGH ? "HIGH" : "LOW"),
                          encoderRaw == LOW ? "yes" : "no");
            return;
        }

        Serial.println("[diag] usage: gpio ui [float]");
        return;
    }

    if(strcmp(cmd, "rs485") == 0 || strcmp(cmd, "RS485") == 0) {
        CommRs485Port port = COMM_RS485_PORT_ONBOARD;
        char* portToken = strtok_r(nullptr, " \t", &savePtr);
        char* saveToken = strtok_r(nullptr, " \t", &savePtr);
        if(!parseRs485PortToken(portToken, &port)) {
            Serial.println("[diag] usage: rs485 <onboard|ex_i2c> [save]");
            Serial.printf("[diag] rs485 current=%s supported_onboard=%s supported_ex_i2c=%s\n",
                          comm_get_rs485_port_label(comm_get_rs485_port()),
                          comm_rs485_port_supported(COMM_RS485_PORT_ONBOARD) ? "yes" : "no",
                          comm_rs485_port_supported(COMM_RS485_PORT_EX_I2C_WORKAROUND) ? "yes" : "no");
            return;
        }

        const bool ok = comm_set_rs485_port(port, true) || comm_get_rs485_port() == port;
        if(ok && saveToken != nullptr && strcmp(saveToken, "save") == 0) {
            pdata_write_rs485_port(static_cast<int16_t>(port));
        }
        Serial.printf("[diag] rs485 set=%s ok=%s saved=%s supported=%s\n",
                      comm_get_rs485_port_label(port),
                      ok ? "yes" : "no",
                      (ok && saveToken != nullptr && strcmp(saveToken, "save") == 0) ? "yes" : "no",
                      comm_rs485_port_supported(port) ? "yes" : "no");
        printStatus();
        return;
    }

    if(strcmp(cmd, "mr") == 0 || strcmp(cmd, "MR") == 0) {
        CommProfile profile;
        uint32_t slave = 0;
        uint32_t reg = 0;
        uint32_t count = 0;
        if(!parseProfileToken(strtok_r(nullptr, " \t", &savePtr), &profile) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &slave) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &reg) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &count)) {
            Serial.println("[diag] usage: mr <j|r> <slave> <reg> <count>");
            return;
        }

        CommDebugTransaction transaction;
        comm_debug_raw_read(profile,
                            static_cast<uint8_t>(slave),
                            static_cast<uint16_t>(reg),
                            static_cast<uint16_t>(count),
                            &transaction);
        printRawTransaction("read",
                            profile,
                            static_cast<uint8_t>(slave),
                            static_cast<uint16_t>(reg),
                            static_cast<uint16_t>(count),
                            "count",
                            transaction);
        return;
    }

    if(strcmp(cmd, "mw") == 0 || strcmp(cmd, "MW") == 0) {
        CommProfile profile;
        uint32_t slave = 0;
        uint32_t reg = 0;
        uint32_t value = 0;
        if(!parseProfileToken(strtok_r(nullptr, " \t", &savePtr), &profile) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &slave) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &reg) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &value)) {
            Serial.println("[diag] usage: mw <j|r> <slave> <reg> <value>");
            return;
        }

        CommDebugTransaction transaction;
        comm_debug_raw_write_single(profile,
                                    static_cast<uint8_t>(slave),
                                    static_cast<uint16_t>(reg),
                                    static_cast<uint16_t>(value),
                                    &transaction);
        printRawTransaction("write",
                            profile,
                            static_cast<uint8_t>(slave),
                            static_cast<uint16_t>(reg),
                            static_cast<uint16_t>(value),
                            "value",
                            transaction);
        return;
    }

    if(strcmp(cmd, "mru") == 0 || strcmp(cmd, "MRU") == 0) {
        uint32_t baud = 0;
        uint32_t serialConfig = 0;
        uint32_t slave = 0;
        uint32_t reg = 0;
        uint32_t count = 0;
        char* baudToken = strtok_r(nullptr, " \t", &savePtr);
        char* modeToken = strtok_r(nullptr, " \t", &savePtr);
        if(!parseUnsignedToken(baudToken, &baud) ||
           !parseSerialConfigToken(modeToken, &serialConfig) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &slave) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &reg) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &count)) {
            Serial.println("[diag] usage: mru <baud> <8N1|8N2|8E1|8E2|8O1|8O2> <slave> <reg> <count>");
            return;
        }

        CommDebugTransaction transaction;
        comm_debug_raw_read_uart(baud,
                                 serialConfig,
                                 static_cast<uint8_t>(slave),
                                 static_cast<uint16_t>(reg),
                                 static_cast<uint16_t>(count),
                                 &transaction);
        printRawTransactionUart("read",
                                baud,
                                modeToken,
                                static_cast<uint8_t>(slave),
                                static_cast<uint16_t>(reg),
                                static_cast<uint16_t>(count),
                                "count",
                                transaction);
        return;
    }

    if(strcmp(cmd, "mwu") == 0 || strcmp(cmd, "MWU") == 0) {
        uint32_t baud = 0;
        uint32_t serialConfig = 0;
        uint32_t slave = 0;
        uint32_t reg = 0;
        uint32_t value = 0;
        char* baudToken = strtok_r(nullptr, " \t", &savePtr);
        char* modeToken = strtok_r(nullptr, " \t", &savePtr);
        if(!parseUnsignedToken(baudToken, &baud) ||
           !parseSerialConfigToken(modeToken, &serialConfig) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &slave) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &reg) ||
           !parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &value)) {
            Serial.println("[diag] usage: mwu <baud> <8N1|8N2|8E1|8E2|8O1|8O2> <slave> <reg> <value>");
            return;
        }

        CommDebugTransaction transaction;
        comm_debug_raw_write_single_uart(baud,
                                         serialConfig,
                                         static_cast<uint8_t>(slave),
                                         static_cast<uint16_t>(reg),
                                         static_cast<uint16_t>(value),
                                         &transaction);
        printRawTransactionUart("write",
                                baud,
                                modeToken,
                                static_cast<uint8_t>(slave),
                                static_cast<uint16_t>(reg),
                                static_cast<uint16_t>(value),
                                "value",
                                transaction);
        return;
    }

    if(strcmp(cmd, "jbaud") == 0 || strcmp(cmd, "JBAUD") == 0) {
        uint32_t baud = 0;
        if(!parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &baud) ||
           (baud != 9600 && baud != 19200 && baud != 38400 && baud != 57600 && baud != 115200)) {
            Serial.println("[diag] usage: jbaud <9600|19200|38400|57600|115200>");
            return;
        }

        if(!motor_jmc_tools_supported()) {
            Serial.println("[diag] JMC baud unavailable for active drive");
            return;
        }

        const bool ok = motor_run_jmc_baud_switch(baud);
        Serial.printf("[diag] jmc baud switch target=%lu ok=%s status=%s result=%lu read_before=%lu read_after=%lu\n",
                      static_cast<unsigned long>(baud),
                      ok ? "yes" : "no",
                      motor_get_jmc_baud_trial_status_text(),
                      static_cast<unsigned long>(motor_get_jmc_baud_trial_result_baud()),
                      static_cast<unsigned long>(motor_get_jmc_baud_trial_read_before()),
                      static_cast<unsigned long>(motor_get_jmc_baud_trial_read_after()));
        printStatus();
        return;
    }

    if(strcmp(cmd, "jb") == 0 || strcmp(cmd, "JB") == 0) {
        uint32_t baud = 0;
        if(!parseUnsignedToken(strtok_r(nullptr, " \t", &savePtr), &baud) ||
           (baud != 9600 && baud != 19200 && baud != 38400 && baud != 57600 && baud != 115200)) {
            Serial.println("[diag] usage: jb <9600|19200|38400|57600|115200>");
            return;
        }

        motor_set_preferred_jmc_baud(baud, true);
        Serial.printf("[diag] preferred JMC boot baud=%lu; redetect or reboot to apply\n",
                      static_cast<unsigned long>(baud));
        printStatus();
        return;
    }

    Serial.printf("[diag] unknown line command: %s\n", cmd);
    printHelp();
}

void logSystemErrorIfChanged() {
    if(error == lastObservedError && !(error == 3 && errorCode != lastObservedFaultCode)) {
        return;
    }

    if(error != 0) {
        const uint32_t packedValue =
            (static_cast<uint32_t>(error) << 16) | static_cast<uint32_t>(errorCode);
        error_history_log(ERROR_HISTORY_SOURCE_SYSTEM,
                          errorLevel(error),
                          ERROR_HISTORY_CODE_SYSTEM_ERROR,
                          packedValue,
                          static_cast<uint8_t>(state));
    }

    lastObservedError = error;
    lastObservedFaultCode = errorCode;
}

void logCommStatusIfChanged() {
    const uint8_t current = static_cast<uint8_t>(commStatus);
    if(current == lastObservedCommStatus) {
        return;
    }

    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      commLevel(commStatus),
                      ERROR_HISTORY_CODE_MOTOR_COMM_STATUS,
                      current,
                      static_cast<uint8_t>(state));
    lastObservedCommStatus = current;
}

void logMotorStatusIfChanged() {
    const uint8_t current = static_cast<uint8_t>(currentStatus);
    if(current == lastObservedMotorStatus) {
        return;
    }

    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      motorLevel(currentStatus),
                      ERROR_HISTORY_CODE_MOTOR_STATUS,
                      current,
                      static_cast<uint8_t>(state));
    lastObservedMotorStatus = current;
}

void logScaleStatusIfChanged() {
    const uint8_t current = static_cast<uint8_t>(scaleStatus);
    if(current == lastObservedScaleStatus) {
        return;
    }

    error_history_log(ERROR_HISTORY_SOURCE_SCALE,
                      scaleLevel(scaleStatus),
                      ERROR_HISTORY_CODE_SCALE_STATUS,
                      current,
                      static_cast<uint8_t>(state));
    lastObservedScaleStatus = current;
}

void handleSerialCommand(char cmd) {
    switch(cmd) {
        case '?':
            printStatus();
            break;
        case 'h':
        case 'H':
            printHelp();
            break;
        case 'v':
        case 'V':
            streamEnabled = !streamEnabled;
            Serial.printf("[diag] stream=%s\n", streamEnabled ? "on" : "off");
            printStatus();
            break;
        case 'g':
        case 'G':
            forceIdleMode(IDLE_GBW);
            Serial.println("[diag] forced state=IDLE_GBW");
            printStatus();
            break;
        case 'i':
        case 'I':
            forceIdleMode(IDLE);
            Serial.println("[diag] forced state=IDLE");
            printStatus();
            break;
        case 'a':
        case 'A':
            pdata_write_drive_type(DRIVE_TYPE_UNKNOWN);
            motor_redetect_drive(false);
            Serial.println("[diag] redetect complete");
            printStatus();
            break;
        case 'j':
        case 'J':
            motor_force_drive_type(DRIVE_TYPE_JMC, true);
            Serial.println("[diag] forced drive=JMC");
            printStatus();
            break;
        case 'r':
        case 'R':
            motor_force_drive_type(DRIVE_TYPE_RT, true);
            Serial.println("[diag] forced drive=RT");
            printStatus();
            break;
        case 'k':
        case 'K':
            motor_force_drive_type(DRIVE_TYPE_KRS_130ST_M07725, true);
            Serial.println("[diag] forced drive=KRS");
            printStatus();
            break;
        case 'x':
        case 'X':
            motor_force_drive_type(DRIVE_TYPE_KRS_XB130AEA20025_SHL, true);
            Serial.println("[diag] forced drive=XB130");
            printStatus();
            break;
        case 's':
        case 'S':
            motor_force_drive_type(DRIVE_TYPE_SEW_MOVILINK, true);
            Serial.println("[diag] forced drive=SEW");
            printStatus();
            break;
        case 'p':
        case 'P':
            printProbeReport();
            break;
        case 'l':
        case 'L':
            dumpErrorHistory(true, false);
            break;
        case '\r':
        case '\n':
            break;
        default:
            Serial.printf("[diag] unknown command=%c\n", cmd);
            printHelp();
            break;
    }
}

}

String diag_api_status_json() {
    String out;
    out.reserve(768);
    appendStatusJson(out);
    return out;
}

String diag_api_snapshot_json() {
    String out;
    out.reserve(8192);
    out += '{';
    out += "\"status\":";
    appendStatusJson(out);
    out += ",\"menus\":[";
    bool firstMenu = true;
    for(uint8_t menuNum = 1; menuNum <= 5; ++menuNum) {
        if(visibleMenuCountByNumber(menuNum) == 0) {
            continue;
        }
        if(!firstMenu) out += ',';
        firstMenu = false;
        appendMenuSnapshotJson(out, menuNum);
    }
    out += "]}";
    return out;
}

bool diag_api_set_menu_value(uint8_t menuNum, uint8_t itemIdx, int32_t value, String* message) {
    const char* rawMessage = nullptr;
    const bool ok = apiSetMenuValue(menuNum, itemIdx, value, &rawMessage);
    if(message != nullptr) {
        *message = rawMessage != nullptr ? rawMessage : "";
    }
    return ok;
}

bool diag_api_set_rpm(int32_t value, String* message) {
    const char* rawMessage = nullptr;
    const bool ok = apiSetRpmValue(value, &rawMessage);
    if(message != nullptr) {
        *message = rawMessage != nullptr ? rawMessage : "";
    }
    return ok;
}

bool diag_api_set_weight(int32_t value, String* message) {
    const char* rawMessage = nullptr;
    const bool ok = apiSetWeightValue(value, &rawMessage);
    if(message != nullptr) {
        *message = rawMessage != nullptr ? rawMessage : "";
    }
    return ok;
}

bool diag_api_run_action(const char* action, const char* arg0, const char* arg1, String* message) {
    const char* rawMessage = "unknown action";
    String dynamicMessage;
    bool ok = false;

    if(action == nullptr) {
        rawMessage = "missing action";
    } else if(strcmp(action, "start") == 0) {
        ok = apiActionStart(&rawMessage);
    } else if(strcmp(action, "stop") == 0) {
        ok = apiActionStop(&rawMessage);
    } else if(strcmp(action, "idle") == 0) {
        ok = apiActionIdle(IDLE, &rawMessage);
    } else if(strcmp(action, "idle_gbw") == 0) {
        ok = apiActionIdle(IDLE_GBW, &rawMessage);
    } else if(strcmp(action, "redetect") == 0) {
        ok = apiActionDrive("auto", &rawMessage);
    } else if(strcmp(action, "drive") == 0) {
        ok = apiActionDrive(arg0, &rawMessage);
    } else if(strcmp(action, "calibrate") == 0) {
        ok = apiActionCalibrate(&rawMessage);
    } else if(strcmp(action, "jmc_baud") == 0) {
        if(arg0 == nullptr) {
            rawMessage = "missing baud";
        } else if(!motor_jmc_tools_supported()) {
            rawMessage = "JMC baud unavailable for active drive";
        } else {
            const uint32_t baud = static_cast<uint32_t>(strtoul(arg0, nullptr, 10));
            ok = motor_run_jmc_baud_switch(baud);
            rawMessage = motor_get_jmc_baud_trial_status_text();
            disp_updateRequired = true;
        }
    } else if(strcmp(action, "reset_network") == 0) {
        ok = apiActionResetNetwork(&rawMessage);
    } else if(strcmp(action, "network_info") == 0) {
        ok = apiActionNetworkInfo(&rawMessage);
    } else if(strcmp(action, "about") == 0) {
        ok = apiActionAbout(&rawMessage);
    } else if(strcmp(action, "reset_defaults") == 0) {
        ok = apiActionResetDefaults(arg0, &rawMessage);
    } else if(strcmp(action, "ota_check") == 0) {
        if(!apiCanStartOta()) {
            rawMessage = "ota requires idle/menu state";
        } else {
            const String source = (arg0 != nullptr && *arg0 != '\0') ? String(arg0) : String("latest");
            ok = apiCheckOtaManifest(source.c_str(), &dynamicMessage);
            rawMessage = dynamicMessage.c_str();
        }
    } else if(strcmp(action, "ota_download_if_available") == 0) {
        if(!apiCanStartOta()) {
            rawMessage = "ota requires idle/menu state";
        } else if(nextOtaSize() == 0) {
            rawMessage = "ota partition unavailable";
        } else {
            const String source = (arg0 != nullptr && *arg0 != '\0') ? String(arg0) : String("latest");
            ok = apiDownloadOtaIfAvailable(source.c_str(), &dynamicMessage);
            rawMessage = dynamicMessage.c_str();
        }
    } else if(strcmp(action, "ota_manifest") == 0 ||
              strcmp(action, "ota_manifest_boot") == 0 ||
              strcmp(action, "ota_manifest_reboot") == 0) {
        if(!apiCanStartOta()) {
            rawMessage = "ota requires idle/menu state";
        } else if(nextOtaSize() == 0) {
            rawMessage = "ota partition unavailable";
        } else {
            const String source = (arg0 != nullptr && *arg0 != '\0') ? String(arg0) : String("latest");
            ok = apiDownloadOtaManifest(source.c_str(), &dynamicMessage);
            if(ok && (strcmp(action, "ota_manifest_boot") == 0 ||
                      strcmp(action, "ota_manifest_reboot") == 0)) {
                const char* bootMessage = nullptr;
                ok = apiSetOtaBootPartition("next", &bootMessage);
                if(dynamicMessage.length() > 0) {
                    dynamicMessage += "; ";
                }
                dynamicMessage += bootMessage != nullptr ? bootMessage : "";
                if(ok && strcmp(action, "ota_manifest_reboot") == 0) {
                    dynamicMessage += "; reboot queued";
                    scheduleApiReboot();
                }
            }
            rawMessage = dynamicMessage.c_str();
        }
    } else if(strcmp(action, "ota_apply_ready") == 0 ||
              strcmp(action, "ota_apply_ready_reboot") == 0) {
        if(!apiCanStartOta()) {
            rawMessage = "ota requires idle/menu state";
        } else {
            ok = controller_ota_mark_ready_bootable(&dynamicMessage);
            if(ok && strcmp(action, "ota_apply_ready_reboot") == 0) {
                dynamicMessage += "; reboot queued";
                scheduleApiReboot();
            }
            rawMessage = dynamicMessage.c_str();
        }
    } else if(strcmp(action, "ota_invalidate_inactive") == 0) {
        ok = controller_ota_invalidate_inactive_slot(&dynamicMessage);
        rawMessage = dynamicMessage.c_str();
    } else if(strcmp(action, "ota_boot_next") == 0) {
        ok = controller_ota_mark_ready_bootable(&dynamicMessage);
        rawMessage = dynamicMessage.c_str();
    } else if(strcmp(action, "ota_reboot") == 0) {
        ok = apiCanStartOta();
        rawMessage = ok ? "reboot queued" : "reboot requires idle/menu state";
        if(ok) {
            scheduleApiReboot();
        }
    } else if(strcmp(action, "stream") == 0) {
        streamEnabled = (arg0 != nullptr && strcmp(arg0, "on") == 0);
        rawMessage = streamEnabled ? "on" : "off";
        ok = true;
    } else if(strcmp(action, "noop") == 0) {
        rawMessage = "ok";
        ok = true;
    }

    if(message != nullptr) {
        *message = rawMessage != nullptr ? rawMessage : "";
        if(arg1 != nullptr && *arg1 != '\0') {
            (void)arg1;
        }
    }
    return ok;
}

void diag_console_init() {
    error_history_init();
    lastObservedError = error;
    lastObservedFaultCode = errorCode;
    lastObservedCommStatus = static_cast<uint8_t>(commStatus);
    lastObservedMotorStatus = static_cast<uint8_t>(currentStatus);
    lastObservedScaleStatus = static_cast<uint8_t>(scaleStatus);
    Serial.printf(
        "[diag] build=%s built=%s %s drive=%s uart=%s firmware=%s\n",
        __FILE__,
        __DATE__,
        __TIME__,
        motor_get_type_label(),
        comm_get_profile_label(),
        sg98_version::buildId().c_str()
    );
    printHelp();
    printStatus();
}

void diag_console_poll() {
    if(pendingApiReboot && static_cast<int32_t>(millis() - pendingApiRebootAt) >= 0) {
        pendingApiReboot = false;
        Serial.println("[diag] api reboot now");
        Serial.flush();
        ESP.restart();
    }

    while(Serial.available() > 0) {
        const int raw = Serial.read();
        if(raw < 0) {
            break;
        }

        const char ch = static_cast<char>(raw);
        if(ch == '\r' || ch == '\n') {
            if(serialLineLen == 0) {
                continue;
            }

            serialLineBuffer[serialLineLen] = '\0';
            if(serialLineLen == 1) {
                handleSerialCommand(serialLineBuffer[0]);
            } else {
                handleLineCommand(serialLineBuffer);
            }
            serialLineLen = 0;
            continue;
        }

        if(serialLineLen + 1 < sizeof(serialLineBuffer)) {
            serialLineBuffer[serialLineLen++] = ch;
        }
    }

    logSystemErrorIfChanged();
    logCommStatusIfChanged();
    logMotorStatusIfChanged();
    logScaleStatusIfChanged();

    if(ble_console_take_dump_request()) {
        dumpBleErrorHistory();
    }

    if(streamEnabled && lastStreamAt + 1000 < millis()) {
        lastStreamAt = millis();
        printStatus();
    }
}
