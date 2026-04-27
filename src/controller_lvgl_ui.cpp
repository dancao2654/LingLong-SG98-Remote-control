#include "controller_lvgl_ui.h"

#include <Arduino.h>
#include <Adafruit_ST7789.h>
#include <lvgl.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "comm.h"
#include "controller_env.h"
#include "gbw.h"
#include "io.h"
#include "main.h"
#include "motor_hal.h"
#include "radio_mode.h"
#include "wifi_ui.h"

namespace {

constexpr int kScreenWidth = 280;
constexpr int kScreenHeight = 240;
constexpr int kDrawBufferLines = 12;
constexpr uint32_t kSplashVisibleMs = 1800;
constexpr uint8_t kVisibleMenuRows = 3;
constexpr uint8_t kVisibleMotorSelectRows = 3;

constexpr uint32_t kColorBg = 0x08111C;
constexpr uint32_t kColorPanel = 0x10233A;
constexpr uint32_t kColorPanelHi = 0x16304D;
constexpr uint32_t kColorBadgeText = 0x071018;
constexpr uint32_t kColorInk = 0xF4FAFF;
constexpr uint32_t kColorSoft = 0xC6D8EA;
constexpr uint32_t kColorMuted = 0xA9BDD1;
constexpr uint32_t kColorLine = 0x244564;
constexpr uint32_t kColorAccent = 0x63C7FF;
constexpr uint32_t kColorMenuItem = 0x0A1726;
constexpr uint32_t kColorMenuItemActive = 0x020812;
constexpr uint32_t kColorMenuItemActiveLine = 0x8ADFFF;
constexpr uint32_t kColorMenuItemActiveText = 0xFFFFFF;
constexpr uint32_t kColorMenuItemText = 0xE6F2FF;
constexpr uint32_t kColorGood = 0x57E89A;
constexpr uint32_t kColorWarn = 0xFFD86A;
constexpr uint32_t kColorDanger = 0xFF7272;
constexpr uint32_t kColorRun = 0x2FD67B;
constexpr uint32_t kColorEspNow = 0x63C7FF;

Adafruit_ST7789* gDisplay = nullptr;
bool gReady = false;
bool gEnabled = true;
uint32_t gLastTickAt = 0;
uint32_t gSplashUntil = 0;
String gSplashLabel;

lv_disp_draw_buf_t gDrawBuffer;
lv_color_t gDrawPixels[kScreenWidth * kDrawBufferLines];
lv_disp_drv_t gDisplayDriver;

struct UiRefs {
    lv_obj_t* root = nullptr;

    lv_obj_t* header = nullptr;
    lv_obj_t* wifiLabel = nullptr;
    lv_obj_t* motorLabel = nullptr;

    lv_obj_t* errorBanner = nullptr;
    lv_obj_t* errorLabel = nullptr;

    lv_obj_t* dashboard = nullptr;
    lv_obj_t* dashMode = nullptr;
    lv_obj_t* dashPrimary = nullptr;
    lv_obj_t* dashSecondary = nullptr;
    lv_obj_t* dashProgressWrap = nullptr;
    lv_obj_t* dashProgress = nullptr;
    lv_obj_t* dashProgressLabel = nullptr;
    lv_obj_t* dashHint = nullptr;
    lv_obj_t* dashFooter = nullptr;
    lv_obj_t* dashLeftCaption = nullptr;
    lv_obj_t* dashLeftStatus = nullptr;
    lv_obj_t* dashRightCaption = nullptr;
    lv_obj_t* dashRightStatus = nullptr;

    lv_obj_t* menuView = nullptr;
    lv_obj_t* menuTitle = nullptr;
    lv_obj_t* menuHelp = nullptr;
    lv_obj_t* menuRows[kVisibleMenuRows] = {};
    lv_obj_t* menuRowLabel[kVisibleMenuRows] = {};
    lv_obj_t* menuRowValue[kVisibleMenuRows] = {};
    lv_obj_t* menuFooter = nullptr;

    lv_obj_t* detailView = nullptr;
    lv_obj_t* detailTitle = nullptr;
    lv_obj_t* detailValue = nullptr;
    lv_obj_t* detailMeta = nullptr;
    lv_obj_t* detailHint = nullptr;

    lv_obj_t* selectView = nullptr;
    lv_obj_t* selectTitle = nullptr;
    lv_obj_t* selectBody = nullptr;
    lv_obj_t* selectRows[kVisibleMotorSelectRows] = {};
    lv_obj_t* selectLabels[kVisibleMotorSelectRows] = {};
    lv_obj_t* selectHints[kVisibleMotorSelectRows] = {};

    lv_obj_t* splashView = nullptr;
    lv_obj_t* splashLabel = nullptr;
} ui;

enum class ActiveView : uint8_t {
    Dashboard,
    Menu,
    Detail,
    Select,
    Splash,
};

ActiveView gActiveView = ActiveView::Dashboard;

lv_color_t lvColor(uint32_t rgb) {
    return lv_color_hex(rgb);
}

uint8_t blendChannel(uint8_t base, uint8_t tint, float mix) {
    if(mix <= 0.0f) return base;
    if(mix >= 1.0f) return tint;
    return static_cast<uint8_t>(static_cast<float>(base) + (static_cast<float>(tint) - static_cast<float>(base)) * mix);
}

uint32_t blendRgb(uint32_t base, uint32_t tint, float mix) {
    const uint8_t baseR = static_cast<uint8_t>((base >> 16) & 0xFF);
    const uint8_t baseG = static_cast<uint8_t>((base >> 8) & 0xFF);
    const uint8_t baseB = static_cast<uint8_t>(base & 0xFF);
    const uint8_t tintR = static_cast<uint8_t>((tint >> 16) & 0xFF);
    const uint8_t tintG = static_cast<uint8_t>((tint >> 8) & 0xFF);
    const uint8_t tintB = static_cast<uint8_t>(tint & 0xFF);
    return (static_cast<uint32_t>(blendChannel(baseR, tintR, mix)) << 16) |
           (static_cast<uint32_t>(blendChannel(baseG, tintG, mix)) << 8) |
           static_cast<uint32_t>(blendChannel(baseB, tintB, mix));
}

struct ViewPalette {
    uint32_t background = kColorBg;
    uint32_t panel = kColorPanel;
    uint32_t panelHi = kColorPanelHi;
    uint32_t line = kColorLine;
};

void setHidden(lv_obj_t* obj, bool hidden) {
    if(obj == nullptr) return;
    if(hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

String fitText(const String& value, size_t maxChars) {
    if(value.length() <= maxChars) return value;
    if(maxChars <= 3) return value.substring(0, maxChars);
    return value.substring(0, maxChars - 3) + "...";
}

const lv_font_t* heroMenuFontForText(const String& value) {
    if((value.indexOf(' ') < 0 && value.length() > 10) || value.length() > 22) {
        return &lv_font_montserrat_24;
    }
    return &lv_font_montserrat_32;
}

String stripMenuName(const char* raw) {
    if(raw == nullptr) return "";
    String out = raw;
    out.replace('\n', ' ');
    out.trim();
    return out;
}

String formatMassMg(int32_t mg) {
    const float grams = static_cast<float>(mg) / 1000.0f;
    char text[20];
    if(fabsf(grams) >= 10.0f) snprintf(text, sizeof(text), "%.1fg", grams);
    else snprintf(text, sizeof(text), "%.2fg", grams);
    return text;
}

String ageLabel(uint32_t timestampMs) {
    if(timestampMs == 0) return "--";
    const uint32_t ageMs = millis() - timestampMs;
    if(ageMs < 1000UL) return "now";
    if(ageMs < 60000UL) return String(ageMs / 1000UL) + "s";
    if(ageMs < 3600000UL) return String(ageMs / 60000UL) + "m";
    return String(ageMs / 3600000UL) + "h";
}

String dateTimeLabel() {
    if(!controller_env_time_valid()) return "--";

    const time_t unixNow = time(nullptr);
    struct tm localTime;
    if(!localtime_r(&unixNow, &localTime)) {
        return "--";
    }

    char text[24];
    snprintf(text,
             sizeof(text),
             "%02d/%02d %02d:%02d",
             localTime.tm_mon + 1,
             localTime.tm_mday,
             localTime.tm_hour,
             localTime.tm_min);
    return String(text);
}

String networkInfoText() {
    String out;
    out.reserve(220);
    out += "Mode  ";
    out += radio_mode_label(radio_mode_current());
    out += "\nSet   ";
    out += radio_mode_value_label(Menu1[Menu1Items::RADIOMODE].value);
    out += "\nSSID  ";
    const String ssid = wifi_ui_sta_ssid();
    out += ssid.length() > 0 ? fitText(ssid, 20) : String("--");
    out += "\nIP    ";
    const String ip = wifi_ui_sta_ip();
    out += ip.length() > 0 ? ip : String("--");
    out += "\nRSSI  ";
    if(wifi_ui_sta_connected()) {
        out += String(wifi_ui_sta_rssi());
        out += " dBm";
    } else {
        out += "--";
    }
    out += "\nTime  ";
    out += dateTimeLabel();
    out += "\nNTP   ";
    out += ageLabel(controller_env_last_ntp_sync_ms());
    out += "\nWx    ";
    if(controller_env_weather_valid()) {
        out += String(controller_env_temperature_f());
        out += "F ";
        out += fitText(controller_env_location(), 13);
        const int8_t precip = controller_env_precipitation_percent();
        if(precip >= 0) {
            out += " ";
            out += String(precip);
            out += "%";
        }
    } else {
        out += "--";
    }
    out += "\nWxAge ";
    out += ageLabel(controller_env_last_weather_fetch_ms());
    return out;
}

String formatPercent(int32_t value) {
    char text[16];
    snprintf(text, sizeof(text), "%ld%%", static_cast<long>(value));
    return text;
}

int32_t clampPct(int32_t value) {
    if(value < 0) return 0;
    if(value > 100) return 100;
    return value;
}

int32_t maxAbs(int32_t a, int32_t b) {
    return abs(a) > abs(b) ? abs(a) : abs(b);
}

int32_t percentOfRange(int32_t value, int32_t minValue, int32_t maxValue) {
    if(maxValue <= minValue) return 0;
    const int32_t clamped = value < minValue ? minValue : (value > maxValue ? maxValue : value);
    return ((clamped - minValue) * 100) / (maxValue - minValue);
}

int32_t currentDisplayMotorRPM() {
    if(motor_currentRPM != 0) return motor_currentRPM;
    switch(state) {
        case GRINDING:
        case PURGING:
        case GRINDING_GBW:
        case CALIBRATING:
            return motor_get_effective_set_rpm();
        default:
            return 0;
    }
}

String driveStatusText() {
    switch(currentStatus) {
        case MOTOR_NOT_CONNECTED: return "Offline";
        case MOTOR_NOT_READY: return "Starting";
        case MOTOR_READY: return "Ready";
        case MOTOR_ENABLED: return "Running";
        case MOTOR_FAULT: return "Fault";
        default: return "Unknown";
    }
}

lv_color_t driveStatusColor() {
    switch(currentStatus) {
        case MOTOR_READY:
        case MOTOR_ENABLED:
            return lvColor(kColorGood);
        case MOTOR_NOT_READY:
            return lvColor(kColorWarn);
        case MOTOR_FAULT:
        case MOTOR_NOT_CONNECTED:
            return lvColor(kColorDanger);
        default:
            return lvColor(kColorInk);
    }
}

lv_color_t scaleStatusColor() {
    switch(scaleStatus) {
        case SCALE_CONNECTED:
            return lvColor(kColorGood);
        case SCALE_CONNECTING:
            return lvColor(kColorWarn);
        case SCALE_LOST:
            return lvColor(kColorDanger);
        case SCALE_DISCONNECTED:
        default:
            return lvColor(kColorAccent);
    }
}

const char* scaleStatusSymbol() {
    switch(scaleStatus) {
        case SCALE_CONNECTED:
            return LV_SYMBOL_OK;
        case SCALE_CONNECTING:
            return LV_SYMBOL_REFRESH;
        case SCALE_LOST:
            return LV_SYMBOL_WARNING;
        case SCALE_DISCONNECTED:
        default:
            return LV_SYMBOL_CLOSE;
    }
}

String scaleStatusTextShort() {
    switch(scaleStatus) {
        case SCALE_CONNECTED: return "Ready";
        case SCALE_CONNECTING: return "Pairing";
        case SCALE_LOST: return "Lost";
        case SCALE_DISCONNECTED:
        default:
            return "Offline";
    }
}

String shortPurgeState() {
    if(state == PURGING) return "Active";
    if(Menu2[AUTO_PURGE_ENABLED].value == 0) return "Off";
    if(ready_purge) return "Ready";
    if(Menu2[SETBUTTONPURGE].value != 0) return "Button";
    return "Auto";
}

lv_color_t purgeStatusColor() {
    if(Menu2[AUTO_PURGE_ENABLED].value == 0) return lvColor(kColorDanger);
    if(state == PURGING) return lvColor(kColorWarn);
    return lvColor(kColorGood);
}

const char* purgeStatusSymbol() {
    if(Menu2[AUTO_PURGE_ENABLED].value == 0) return LV_SYMBOL_CLOSE;
    if(state == PURGING) return LV_SYMBOL_REFRESH;
    return LV_SYMBOL_OK;
}

String shortScaleState() {
    if(scaleStatus == SCALE_CONNECTED) return formatMassMg(currentWeight);
    return scaleStatusTextShort();
}

lv_color_t torqueStatusColor() {
    const int32_t torque = clampPct(motor_get_torque_percent());
    if(torque >= 85) return lvColor(kColorDanger);
    if(torque >= 60) return lvColor(kColorWarn);
    return lvColor(kColorAccent);
}

lv_color_t rpmStatusColor() {
    return lvColor(kColorAccent);
}

lv_color_t grindingRpmColor() {
    return lvColor(motor_is_direction_reversed() ? kColorWarn : kColorRun);
}

const char* driveStatusSymbol() {
    switch(currentStatus) {
        case MOTOR_READY:
        case MOTOR_ENABLED:
            return LV_SYMBOL_OK;
        case MOTOR_NOT_READY:
            return LV_SYMBOL_REFRESH;
        case MOTOR_FAULT:
            return LV_SYMBOL_WARNING;
        case MOTOR_NOT_CONNECTED:
        default:
            return LV_SYMBOL_CLOSE;
    }
}

const char* getErrorChar(uint8_t errorCode) {
    switch(errorCode) {
        case 1: return "Drive connection failed";
        case 2: return "Drive connection lost";
        case 3: return "Drive fault";
        case 4: return "Drive packet loss > 25";
        case 5: return "Failed to create mutex";
        case 6: return "Motor stalled";
        case 7: return "Write error, reboot all";
        case 8: return "First use, reboot drive";
        case 100: return "Unexpected set RPM";
        case 101: return "No drive connection";
        case 102: return "Calibration cancelled";
        case 103: return "Could not load stored data";
        case 104: return "Scale connection lost";
        case 105: return "No output after 3s";
        case 106: return "No scale connected";
        case 107: return "Please reboot drive";
        default: return "Unknown error";
    }
}

String headerLinkLabel() {
    if(radio_mode_is_espnow()) return "REMOTE LINK";
    if(!wifi_ui_is_active()) return "WI-FI OFF";
    return wifi_ui_sta_connected() ? "WI-FI LAN" : "WI-FI AP";
}

String headerMotorLabel() {
    String label = motor_get_type_label();
    label.trim();
    if(label.length() == 0 || label.equalsIgnoreCase("unknown")) return "AUTO";
    label.toUpperCase();
    return label;
}

lv_color_t headerLinkColor() {
    if(!radio_mode_status_ok()) {
        return lvColor(kColorWarn);
    }
    return radio_mode_is_espnow() ? lvColor(kColorEspNow) : lvColor(kColorGood);
}

uint32_t headerLinkFillColor() {
    if(!radio_mode_status_ok()) {
        return blendRgb(kColorPanelHi, kColorWarn, 0.50f);
    }
    return radio_mode_is_espnow()
               ? blendRgb(kColorPanelHi, kColorEspNow, 0.54f)
               : blendRgb(kColorPanelHi, kColorGood, 0.56f);
}

lv_color_t headerMotorColor() {
    switch(motor_get_drive_type()) {
        case DRIVE_TYPE_JMC: return lvColor(kColorAccent);
        case DRIVE_TYPE_RT: return lvColor(kColorGood);
        case DRIVE_TYPE_KRS_130ST_M07725: return lvColor(kColorWarn);
        case DRIVE_TYPE_UNKNOWN:
        default:
            return lvColor(kColorMuted);
    }
}

uint32_t headerMotorFillColor() {
    switch(motor_get_drive_type()) {
        case DRIVE_TYPE_JMC:
            return blendRgb(kColorPanelHi, kColorWarn, 0.52f);
        case DRIVE_TYPE_RT:
            return blendRgb(kColorPanelHi, kColorGood, 0.56f);
        case DRIVE_TYPE_KRS_130ST_M07725:
            return blendRgb(kColorPanelHi, kColorWarn, 0.54f);
        case DRIVE_TYPE_UNKNOWN:
        default:
            return blendRgb(kColorPanelHi, kColorAccent, 0.50f);
    }
}

const menuEntry* menuByNum(int menuNum) {
    switch(menuNum) {
        case 1: return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        case 4: return Menu4;
        case 5: return Menu5;
        default: return nullptr;
    }
}

String menuTitleForNum(int menuNum) {
    if(menuNum == 5) return "Wireless";
    return "Grinder Setup";
}

String menuPathForNum(int menuNum) {
    switch(menuNum) {
        case 1: return "Only grinder setup. Rotate to browse.";
        case 2: return "Purge Settings. Rotate to browse.";
        case 3: return "GbW Settings. Rotate to browse.";
        case 4: return "JMC Baud. Rotate to browse.";
        case 5: return "Wireless. Rotate to browse.";
        default: return "Rotate to browse.";
    }
}

int selectedIndexForMenuState() {
    switch(state) {
        case MENU1: return menu1Selected;
        case MENU2: return menu2Selected;
        case MENU3: return menu3Selected;
        case MENU4: return menu4Selected;
        case MENU5: return menu5Selected;
        default: return 0;
    }
}

int menuCountForState() {
    switch(state) {
        case MENU1: return NUM_MENU1_ITEMS;
        case MENU2: return NUM_MENU2_ITEMS;
        case MENU3: return NUM_MENU3_ITEMS;
        case MENU4: return NUM_MENU4_ITEMS;
        case MENU5: return NUM_MENU5_ITEMS;
        default: return 0;
    }
}

int visibleMenuCountFor(int menuNum, int rawCount) {
    int count = 0;
    for(int idx = 0; idx < rawCount; ++idx) {
        if(motor_menu_item_visible(static_cast<uint8_t>(menuNum), static_cast<uint8_t>(idx))) {
            ++count;
        }
    }
    return count;
}

int menuIndexForVisibleOrdinal(int menuNum, int rawCount, int ordinal) {
    int seen = 0;
    for(int idx = 0; idx < rawCount; ++idx) {
        if(!motor_menu_item_visible(static_cast<uint8_t>(menuNum), static_cast<uint8_t>(idx))) {
            continue;
        }
        if(seen == ordinal) {
            return idx;
        }
        ++seen;
    }
    return 0;
}

int visibleOrdinalForMenuIndex(int menuNum, int rawCount, int selectedIdx) {
    int seen = 0;
    for(int idx = 0; idx < rawCount; ++idx) {
        if(!motor_menu_item_visible(static_cast<uint8_t>(menuNum), static_cast<uint8_t>(idx))) {
            continue;
        }
        if(idx == selectedIdx) {
            return seen;
        }
        ++seen;
    }
    return 0;
}

int menuNumForState() {
    switch(state) {
        case MENU1: return 1;
        case MENU2: return 2;
        case MENU3: return 3;
        case MENU4: return 4;
        case MENU5: return 5;
        default: return 0;
    }
}

String toggleValueText(int menuNum, int selectedIdx, int32_t value) {
    if((menuNum == 1 && selectedIdx == Menu1Items::RADIOMODE) ||
       (menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_RADIO_MODE)) {
        return radio_mode_value_label(value);
    }
    if(menuNum == 1 && selectedIdx == Menu1Items::SETMOTORDIRECTION) {
        return value == 0 ? "Normal" : "Reversed";
    }
    if(menuNum == 3 && selectedIdx == Menu3Items::SAVE_SCALE) {
        return value == 0 ? "Do not save" : "Save";
    }
    if(menuNum == 1 && (selectedIdx == Menu1Items::RESET || selectedIdx == Menu1Items::RESETNETWORK)) {
        return value == 0 ? "Back" : "Reset";
    }
    return value == 0 ? "Disabled" : "Enabled";
}

const MotorCommandDescriptor* motorCommandForMenuItem(int menuNum, int selectedIdx) {
    if(menuNum <= 0 || selectedIdx < 0) return nullptr;
    return motor_get_current_command_descriptor_for_menu(static_cast<uint8_t>(menuNum),
                                                         static_cast<uint8_t>(selectedIdx));
}

String menuItemLabel(const menuEntry& item, int menuNum, int selectedIdx) {
    if(menuNum == 1 && selectedIdx == Menu1Items::EXITMENU) return "Exit Setup";
    if(menuNum > 1 && selectedIdx == 0) return "Back to Setup";

    const MotorCommandDescriptor* command = motorCommandForMenuItem(menuNum, selectedIdx);
    if(command != nullptr && command->label != nullptr && command->label[0] != '\0') {
        return command->label;
    }
    return stripMenuName(item.name);
}

String menuItemSummary(const menuEntry& item, int menuNum, int selectedIdx) {
    const MotorCommandDescriptor* command = motorCommandForMenuItem(menuNum, selectedIdx);
    if(command != nullptr && !command->supported) return "N/A";
    if(menuNum == 1 && selectedIdx == Menu1Items::RADIOMODE) {
        return "Open";
    }
    if(menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_RADIO_MODE) {
        return radio_mode_value_label(item.value);
    }

    if(item.scalar == 0 && item.maxValue == 0 && item.minValue == 0) {
        if(menuNum == 1 && selectedIdx == Menu1Items::EXITMENU) return "Close";
        if(menuNum > 1 && selectedIdx == 0) return "Up";
        if(menuNum == 1 &&
           (selectedIdx == PURGESETTINGS ||
            selectedIdx == GBWSETTINGS ||
            selectedIdx == JMCBAUDMENU ||
            selectedIdx == DRIVESETTINGS ||
            selectedIdx == RADIOMODE ||
            selectedIdx == NETWORKINFO)) return "Open";
        if(menuNum == 5 &&
           (selectedIdx == Menu5Items::WIRELESS_NETWORK_INFO ||
            selectedIdx == Menu5Items::WIRELESS_RESET_NETWORK)) return "Open";
        if(menuNum == 4 && isJmcBaudMenuItem(selectedIdx)) return String(baudForJmcMenuItem(selectedIdx));
        if(menuNum == 1 && selectedIdx == CALIBRATE) return "Hold";
        return "Select";
    }

    if(item.minValue == 0 && item.maxValue == 1 && item.scalar == 1) {
        return toggleValueText(menuNum, selectedIdx, item.value);
    }

    String value = String(item.value);
    const char* unit = item.unit;
    if(command != nullptr && command->unit != nullptr) {
        unit = command->unit;
    }
    if(unit != nullptr && unit[0] != '\0') value += unit;
    return value;
}

String menuDetailValue(const menuEntry& item, int menuNum, int selectedIdx) {
    if(menuNum == 4 && isJmcBaudMenuItem(selectedIdx)) return String(baudForJmcMenuItem(selectedIdx));
    if(item.scalar == 0 && item.maxValue == 0 && item.minValue == 0) {
        if(menuNum == 1 && selectedIdx == CALIBRATE) return "Ready";
        return "Select";
    }
    return menuItemSummary(item, menuNum, selectedIdx);
}

String menuDetailMeta(const menuEntry& item, int menuNum, int selectedIdx) {
    const MotorCommandDescriptor* command = motorCommandForMenuItem(menuNum, selectedIdx);
    if(command != nullptr) {
        if(!command->supported) return String(motor_get_type_label()) + " does not use this control";
        if(command->helpText != nullptr && command->helpText[0] != '\0') return command->helpText;
    }
    if(menuNum == 1 && selectedIdx == Menu1Items::CALIBRATE) return "Hold encoder to start";
    if(menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_NETWORK_INFO) return "Shows cached network and sync status";
    if(menuNum == 4 && isJmcBaudMenuItem(selectedIdx)) return "Writes drive baud for next boot";
    if(menuNum == 1 && selectedIdx == Menu1Items::RADIOMODE) return "Wireless mode, status, and reset";
    if(menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_RADIO_MODE) return "WiFi opens setup, ESP-NOW parks low power";
    if(menuNum == 1 && selectedIdx == Menu1Items::RESET) return "Reset needs encoder confirm";
    if(menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_RESET_NETWORK) return "Clears saved WiFi and setup state";
    if(item.minValue == 0 && item.maxValue == 1 && item.scalar == 1) return "Rotate to choose";
    if(item.scalar == 0 && item.maxValue == 0 && item.minValue == 0) return "Encoder opens";
    return "Rotate to adjust";
}

String menuDetailHint(const menuEntry& item, int menuNum, int selectedIdx) {
    const MotorCommandDescriptor* command = motorCommandForMenuItem(menuNum, selectedIdx);
    if(command != nullptr) {
        if(!command->supported) return motor_get_type_guidance();
        if(command->guidance != nullptr && command->guidance[0] != '\0') return command->guidance;
    }
    if(menuNum == 4 && isJmcBaudMenuItem(selectedIdx)) return "Encoder writes. Power cycle drive after the change.";
    if(menuNum == 1 && selectedIdx == Menu1Items::RADIOMODE) return "Encoder opens. Start goes back.";
    if(menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_RADIO_MODE) return "Radio Off is local-only standby.";
    if(menuNum == 5 && selectedIdx == Menu5Items::WIRELESS_NETWORK_INFO) return "Start or encoder goes back.";
    if(menuNum == 1 && selectedIdx == Menu1Items::CALIBRATE) return "Start cancels and goes back.";
    if(item.scalar == 0 && item.maxValue == 0 && item.minValue == 0) return "Start goes back.";
    return "Encoder saves. Start cancels.";
}

const char* motorSelectTitle(int idx) {
    switch(idx) {
        case MOTOR_SELECT_AUTO: return "AUTO";
        case MOTOR_SELECT_JMC: return "JMC";
        case MOTOR_SELECT_RT: return "RT";
        case MOTOR_SELECT_KRS_130ST_M07725: return "KRS";
        default: return "";
    }
}

const char* motorSelectHint(int idx) {
    switch(idx) {
        case MOTOR_SELECT_AUTO: return "Re-detect";
        case MOTOR_SELECT_JMC: return "Force JMC";
        case MOTOR_SELECT_RT: return "Force RT";
        case MOTOR_SELECT_KRS_130ST_M07725: return "Force KRS 130ST";
        default: return "";
    }
}

void styleClear(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

void stylePanel(lv_obj_t* obj, uint32_t fill, uint32_t line, lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_set_style_bg_color(obj, lvColor(fill), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lvColor(line), 0);
    lv_obj_set_style_radius(obj, 18, 0);
}

void styleBadge(lv_obj_t* obj, uint32_t fill, uint32_t textColor) {
    lv_obj_set_style_bg_color(obj, lvColor(fill), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, lvColor(textColor), 0);
    lv_obj_set_style_radius(obj, 11, 0);
    lv_obj_set_style_pad_left(obj, 10, 0);
    lv_obj_set_style_pad_right(obj, 10, 0);
    lv_obj_set_style_pad_top(obj, 3, 0);
    lv_obj_set_style_pad_bottom(obj, 3, 0);
}

uint32_t currentViewTint() {
    if(error != 0 || currentStatus == MOTOR_FAULT || currentStatus == MOTOR_NOT_CONNECTED) {
        return kColorDanger;
    }

    switch(state) {
        case IDLE_GBW:
        case GRINDING_GBW:
            return kColorGood;
        case GRINDING:
            return kColorRun;
        case PURGING:
        case CALIBRATING:
            return kColorWarn;
        case MENU2:
            return kColorWarn;
        case MENU3:
            return kColorGood;
        case MENU5:
        case MOTOR_SELECT:
        case NETWORK_INFO:
            return kColorAccent;
        case MENU1:
        case MENU4:
        case IDLE:
        default:
            return kColorAccent;
    }
}

ViewPalette buildViewPalette() {
    const uint32_t tint = currentViewTint();
    ViewPalette palette = {};
    palette.background = blendRgb(kColorBg, tint, 0.10f);
    palette.panel = blendRgb(kColorPanel, tint, 0.14f);
    palette.panelHi = blendRgb(kColorPanelHi, tint, 0.18f);
    palette.line = blendRgb(kColorLine, tint, 0.24f);
    return palette;
}

void applyViewPalette() {
    const ViewPalette palette = buildViewPalette();
    lv_obj_set_style_bg_color(ui.root, lvColor(palette.background), 0);

    stylePanel(ui.dashboard, palette.panel, palette.line);
    stylePanel(ui.dashProgressWrap, palette.panelHi, palette.line);
    stylePanel(ui.dashFooter, blendRgb(palette.panelHi, palette.background, 0.18f), palette.line, LV_OPA_COVER);

    stylePanel(ui.menuView, palette.panel, palette.line);
    stylePanel(ui.detailView, palette.panel, palette.line);
    stylePanel(ui.selectView, palette.panel, palette.line);
    stylePanel(ui.splashView, blendRgb(palette.panelHi, palette.background, 0.14f), palette.line);
}

void showView(ActiveView view) {
    gActiveView = view;
    setHidden(ui.dashboard, view != ActiveView::Dashboard);
    setHidden(ui.menuView, view != ActiveView::Menu);
    setHidden(ui.detailView, view != ActiveView::Detail);
    setHidden(ui.selectView, view != ActiveView::Select);
    setHidden(ui.splashView, view != ActiveView::Splash);

    const bool splash = view == ActiveView::Splash;
    setHidden(ui.header, splash);
    setHidden(ui.errorBanner, splash || error == 0);
}

void createHeader() {
    ui.header = lv_obj_create(ui.root);
    styleClear(ui.header);
    lv_obj_set_size(ui.header, 248, 26);
    lv_obj_set_pos(ui.header, 16, 8);

    ui.wifiLabel = lv_label_create(ui.header);
    lv_obj_set_width(ui.wifiLabel, 114);
    lv_obj_set_style_text_font(ui.wifiLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(ui.wifiLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.wifiLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.wifiLabel, LV_ALIGN_LEFT_MID, 0, 0);
    styleBadge(ui.wifiLabel, headerLinkFillColor(), kColorBadgeText);

    ui.motorLabel = lv_label_create(ui.header);
    lv_obj_set_width(ui.motorLabel, 92);
    lv_obj_set_style_text_font(ui.motorLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ui.motorLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.motorLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.motorLabel, LV_ALIGN_RIGHT_MID, 0, 0);
    styleBadge(ui.motorLabel, headerMotorFillColor(), kColorBadgeText);
}

void createErrorBanner() {
    ui.errorBanner = lv_obj_create(ui.root);
    stylePanel(ui.errorBanner, 0x38161A, 0x663038);
    lv_obj_set_size(ui.errorBanner, 264, 22);
    lv_obj_set_pos(ui.errorBanner, 8, 33);
    lv_obj_set_style_pad_hor(ui.errorBanner, 10, 0);
    lv_obj_set_style_pad_ver(ui.errorBanner, 2, 0);

    ui.errorLabel = lv_label_create(ui.errorBanner);
    lv_obj_set_width(ui.errorLabel, 244);
    lv_obj_set_style_text_font(ui.errorLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.errorLabel, lvColor(kColorDanger), 0);
    lv_label_set_long_mode(ui.errorLabel, LV_LABEL_LONG_DOT);
    lv_obj_center(ui.errorLabel);
    setHidden(ui.errorBanner, true);
}

void createDashboardUi() {
    ui.dashboard = lv_obj_create(ui.root);
    stylePanel(ui.dashboard, kColorPanel, kColorLine);
    lv_obj_set_size(ui.dashboard, 264, 176);
    lv_obj_set_pos(ui.dashboard, 8, 56);
    lv_obj_set_style_pad_all(ui.dashboard, 12, 0);

    ui.dashMode = lv_label_create(ui.dashboard);
    lv_obj_set_style_text_font(ui.dashMode, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.dashMode, lvColor(kColorAccent), 0);
    lv_obj_align(ui.dashMode, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.dashPrimary = lv_label_create(ui.dashboard);
    lv_obj_set_width(ui.dashPrimary, 240);
    lv_obj_set_style_text_font(ui.dashPrimary, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ui.dashPrimary, lvColor(kColorInk), 0);
    lv_label_set_long_mode(ui.dashPrimary, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashPrimary, LV_ALIGN_TOP_LEFT, 0, 18);

    ui.dashSecondary = lv_label_create(ui.dashboard);
    lv_obj_set_width(ui.dashSecondary, 240);
    lv_obj_set_style_text_font(ui.dashSecondary, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ui.dashSecondary, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.dashSecondary, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashSecondary, LV_ALIGN_TOP_LEFT, 0, 62);

    ui.dashProgressWrap = lv_obj_create(ui.dashboard);
    stylePanel(ui.dashProgressWrap, kColorPanelHi, kColorLine);
    lv_obj_set_size(ui.dashProgressWrap, 240, 30);
    lv_obj_align(ui.dashProgressWrap, LV_ALIGN_TOP_LEFT, 0, 90);
    lv_obj_set_style_pad_hor(ui.dashProgressWrap, 10, 0);
    lv_obj_set_style_pad_ver(ui.dashProgressWrap, 8, 0);

    ui.dashProgress = lv_bar_create(ui.dashProgressWrap);
    lv_obj_set_size(ui.dashProgress, 142, 8);
    lv_obj_align(ui.dashProgress, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(ui.dashProgress, lvColor(0x0C1B2C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.dashProgress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.dashProgress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.dashProgress, lvColor(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.dashProgress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    ui.dashProgressLabel = lv_label_create(ui.dashProgressWrap);
    lv_obj_set_width(ui.dashProgressLabel, 74);
    lv_obj_set_style_text_font(ui.dashProgressLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.dashProgressLabel, lvColor(kColorInk), 0);
    lv_obj_set_style_text_align(ui.dashProgressLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(ui.dashProgressLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashProgressLabel, LV_ALIGN_RIGHT_MID, 0, 0);

    ui.dashHint = lv_label_create(ui.dashboard);
    lv_obj_set_width(ui.dashHint, 240);
    lv_obj_set_style_text_font(ui.dashHint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.dashHint, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.dashHint, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashHint, LV_ALIGN_TOP_LEFT, 0, 120);

    ui.dashFooter = lv_obj_create(ui.dashboard);
    stylePanel(ui.dashFooter, 0x0C1B2C, kColorLine, LV_OPA_COVER);
    lv_obj_set_size(ui.dashFooter, 240, 30);
    lv_obj_align(ui.dashFooter, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_pad_hor(ui.dashFooter, 12, 0);
    lv_obj_set_style_pad_ver(ui.dashFooter, 4, 0);

    ui.dashLeftCaption = lv_label_create(ui.dashFooter);
    lv_obj_set_width(ui.dashLeftCaption, 28);
    lv_obj_set_style_text_font(ui.dashLeftCaption, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.dashLeftCaption, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.dashLeftCaption, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashLeftCaption, LV_ALIGN_LEFT_MID, 0, 0);

    ui.dashLeftStatus = lv_label_create(ui.dashFooter);
    lv_obj_set_width(ui.dashLeftStatus, 18);
    lv_obj_set_style_text_font(ui.dashLeftStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ui.dashLeftStatus, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ui.dashLeftStatus, lvColor(kColorAccent), 0);
    lv_label_set_long_mode(ui.dashLeftStatus, LV_LABEL_LONG_DOT);
    lv_obj_align_to(ui.dashLeftStatus, ui.dashLeftCaption, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    ui.dashRightCaption = lv_label_create(ui.dashFooter);
    lv_obj_set_width(ui.dashRightCaption, 40);
    lv_obj_set_style_text_font(ui.dashRightCaption, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.dashRightCaption, lvColor(kColorSoft), 0);
    lv_obj_set_style_text_align(ui.dashRightCaption, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(ui.dashRightCaption, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashRightCaption, LV_ALIGN_RIGHT_MID, -22, 0);

    ui.dashRightStatus = lv_label_create(ui.dashFooter);
    lv_obj_set_width(ui.dashRightStatus, 18);
    lv_obj_set_style_text_font(ui.dashRightStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ui.dashRightStatus, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ui.dashRightStatus, lvColor(kColorAccent), 0);
    lv_label_set_long_mode(ui.dashRightStatus, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.dashRightStatus, LV_ALIGN_RIGHT_MID, 0, 0);
}

void createMenuUi() {
    ui.menuView = lv_obj_create(ui.root);
    stylePanel(ui.menuView, kColorPanel, kColorLine);
    lv_obj_set_size(ui.menuView, 264, 176);
    lv_obj_set_pos(ui.menuView, 8, 56);
    lv_obj_set_style_pad_all(ui.menuView, 12, 0);

    ui.menuTitle = lv_label_create(ui.menuView);
    lv_obj_set_width(ui.menuTitle, 240);
    lv_obj_set_style_text_font(ui.menuTitle, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ui.menuTitle, lvColor(kColorInk), 0);
    lv_obj_align(ui.menuTitle, LV_ALIGN_TOP_LEFT, 0, 0);
    setHidden(ui.menuTitle, true);

    ui.menuHelp = lv_label_create(ui.menuView);
    lv_obj_set_width(ui.menuHelp, 240);
    lv_obj_set_style_text_font(ui.menuHelp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.menuHelp, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.menuHelp, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.menuHelp, LV_ALIGN_TOP_LEFT, 0, 24);
    setHidden(ui.menuHelp, true);

    for(uint8_t i = 0; i < kVisibleMenuRows; ++i) {
        const bool hero = i == 1;
        const int16_t rowH = hero ? 82 : 30;
        const int16_t rowY = i == 0 ? 0 : (hero ? 35 : 122);
        ui.menuRows[i] = lv_obj_create(ui.menuView);
        stylePanel(ui.menuRows[i], kColorMenuItem, kColorLine, LV_OPA_90);
        lv_obj_set_size(ui.menuRows[i], hero ? 240 : 220, rowH);
        lv_obj_set_pos(ui.menuRows[i], hero ? 0 : 10, rowY);
        lv_obj_set_style_pad_hor(ui.menuRows[i], hero ? 16 : 10, 0);
        lv_obj_set_style_pad_ver(ui.menuRows[i], hero ? 12 : 4, 0);

        ui.menuRowLabel[i] = lv_label_create(ui.menuRows[i]);
        lv_obj_set_width(ui.menuRowLabel[i], hero ? 208 : 190);
        lv_obj_set_style_text_font(ui.menuRowLabel[i], hero ? &lv_font_montserrat_32 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui.menuRowLabel[i], lvColor(kColorMenuItemText), 0);
        lv_obj_set_style_text_align(ui.menuRowLabel[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(ui.menuRowLabel[i], 2, 0);
        lv_label_set_long_mode(ui.menuRowLabel[i], hero ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
        lv_obj_center(ui.menuRowLabel[i]);

        ui.menuRowValue[i] = lv_label_create(ui.menuRows[i]);
        lv_obj_set_width(ui.menuRowValue[i], 1);
        lv_obj_set_style_text_font(ui.menuRowValue[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui.menuRowValue[i], lvColor(kColorSoft), 0);
        lv_obj_set_style_text_align(ui.menuRowValue[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(ui.menuRowValue[i], LV_LABEL_LONG_DOT);
        lv_obj_align(ui.menuRowValue[i], LV_ALIGN_RIGHT_MID, 0, 0);
        setHidden(ui.menuRowValue[i], true);
    }

    ui.menuFooter = lv_label_create(ui.menuView);
    lv_obj_set_width(ui.menuFooter, 240);
    lv_obj_set_style_text_font(ui.menuFooter, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.menuFooter, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.menuFooter, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.menuFooter, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    setHidden(ui.menuFooter, true);
}

void createDetailUi() {
    ui.detailView = lv_obj_create(ui.root);
    stylePanel(ui.detailView, kColorPanel, kColorLine);
    lv_obj_set_size(ui.detailView, 264, 176);
    lv_obj_set_pos(ui.detailView, 8, 56);
    lv_obj_set_style_pad_all(ui.detailView, 12, 0);

    ui.detailTitle = lv_label_create(ui.detailView);
    lv_obj_set_width(ui.detailTitle, 240);
    lv_obj_set_style_text_font(ui.detailTitle, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ui.detailTitle, lvColor(kColorInk), 0);
    lv_label_set_long_mode(ui.detailTitle, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.detailTitle, LV_ALIGN_TOP_LEFT, 0, 0);
    setHidden(ui.detailTitle, true);

    lv_obj_t* card = lv_obj_create(ui.detailView);
    stylePanel(card, kColorMenuItemActive, kColorMenuItemActiveLine);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_size(card, 240, 140);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 6);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 16, 0);

    ui.detailValue = lv_label_create(card);
    lv_obj_set_width(ui.detailValue, 208);
    lv_obj_set_style_text_font(ui.detailValue, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ui.detailValue, lvColor(kColorMenuItemActiveText), 0);
    lv_obj_set_style_text_align(ui.detailValue, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(ui.detailValue, 2, 0);
    lv_label_set_long_mode(ui.detailValue, LV_LABEL_LONG_WRAP);
    lv_obj_center(ui.detailValue);

    ui.detailMeta = lv_label_create(ui.detailView);
    lv_obj_set_width(ui.detailMeta, 240);
    lv_obj_set_style_text_font(ui.detailMeta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui.detailMeta, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.detailMeta, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.detailMeta, LV_ALIGN_TOP_LEFT, 0, 122);
    setHidden(ui.detailMeta, true);

    ui.detailHint = lv_label_create(ui.detailView);
    lv_obj_set_width(ui.detailHint, 240);
    lv_obj_set_style_text_font(ui.detailHint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.detailHint, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.detailHint, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui.detailHint, LV_ALIGN_TOP_LEFT, 0, 146);
    setHidden(ui.detailHint, true);
}

void createMotorSelectUi() {
    ui.selectView = lv_obj_create(ui.root);
    stylePanel(ui.selectView, kColorPanel, kColorLine);
    lv_obj_set_size(ui.selectView, 264, 176);
    lv_obj_set_pos(ui.selectView, 8, 56);
    lv_obj_set_style_pad_all(ui.selectView, 12, 0);

    ui.selectTitle = lv_label_create(ui.selectView);
    lv_obj_set_width(ui.selectTitle, 240);
    lv_obj_set_style_text_font(ui.selectTitle, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ui.selectTitle, lvColor(kColorInk), 0);
    lv_obj_align(ui.selectTitle, LV_ALIGN_TOP_LEFT, 0, 0);
    setHidden(ui.selectTitle, true);

    ui.selectBody = lv_label_create(ui.selectView);
    lv_obj_set_width(ui.selectBody, 240);
    lv_obj_set_style_text_font(ui.selectBody, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui.selectBody, lvColor(kColorSoft), 0);
    lv_label_set_long_mode(ui.selectBody, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.selectBody, LV_ALIGN_TOP_LEFT, 0, 24);
    setHidden(ui.selectBody, true);

    for(uint8_t i = 0; i < kVisibleMotorSelectRows; ++i) {
        ui.selectRows[i] = lv_obj_create(ui.selectView);
        stylePanel(ui.selectRows[i], kColorMenuItem, kColorLine);
        lv_obj_set_size(ui.selectRows[i], i == 1 ? 240 : 1, i == 1 ? 140 : 1);
        lv_obj_set_pos(ui.selectRows[i], i == 1 ? 0 : 0, i == 1 ? 6 : 0);
        lv_obj_set_style_pad_hor(ui.selectRows[i], i == 1 ? 16 : 0, 0);
        lv_obj_set_style_pad_ver(ui.selectRows[i], i == 1 ? 16 : 0, 0);

        ui.selectLabels[i] = lv_label_create(ui.selectRows[i]);
        lv_obj_set_width(ui.selectLabels[i], i == 1 ? 208 : 1);
        lv_obj_set_style_text_font(ui.selectLabels[i], i == 1 ? &lv_font_montserrat_32 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ui.selectLabels[i], lvColor(kColorMenuItemText), 0);
        lv_obj_set_style_text_align(ui.selectLabels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(ui.selectLabels[i], i == 1 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
        lv_obj_center(ui.selectLabels[i]);

        ui.selectHints[i] = lv_label_create(ui.selectRows[i]);
        lv_obj_set_style_text_font(ui.selectHints[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ui.selectHints[i], lvColor(kColorSoft), 0);
        lv_obj_align(ui.selectHints[i], LV_ALIGN_RIGHT_MID, 0, 0);
        setHidden(ui.selectHints[i], true);
    }
}

void createSplashUi() {
    ui.splashView = lv_obj_create(ui.root);
    stylePanel(ui.splashView, 0x0B1E31, 0x244564);
    lv_obj_set_size(ui.splashView, 264, 176);
    lv_obj_set_pos(ui.splashView, 8, 56);

    ui.splashLabel = lv_label_create(ui.splashView);
    lv_obj_set_width(ui.splashLabel, 240);
    lv_obj_set_style_text_font(ui.splashLabel, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ui.splashLabel, lvColor(kColorWarn), 0);
    lv_obj_set_style_text_align(ui.splashLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.splashLabel, LV_LABEL_LONG_WRAP);
    lv_obj_center(ui.splashLabel);
}

void createUi() {
    ui.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ui.root, kScreenWidth, kScreenHeight);
    lv_obj_set_pos(ui.root, 0, 0);
    lv_obj_set_style_bg_color(ui.root, lvColor(kColorBg), 0);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.root, 0, 0);
    lv_obj_set_style_radius(ui.root, 0, 0);
    lv_obj_set_style_pad_all(ui.root, 0, 0);
    lv_obj_clear_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);

    createHeader();
    createErrorBanner();
    createDashboardUi();
    createMenuUi();
    createDetailUi();
    createMotorSelectUi();
    createSplashUi();

    showView(ActiveView::Dashboard);
}

void flushDisplay(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* colorMap) {
    if(gDisplay == nullptr || !gEnabled) {
        lv_disp_flush_ready(drv);
        return;
    }

    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    if(width <= 0 || height <= 0) {
        lv_disp_flush_ready(drv);
        return;
    }

    gDisplay->startWrite();
    gDisplay->setAddrWindow(area->x1, area->y1, width, height);
    gDisplay->writePixels(reinterpret_cast<uint16_t*>(colorMap),
                          static_cast<uint32_t>(width * height),
                          true);
    gDisplay->endWrite();
    lv_disp_flush_ready(drv);
}

void syncHeader() {
    const String wifiText = String(LV_SYMBOL_WIFI) + " " + fitText(headerLinkLabel(), 9);
    lv_label_set_text(ui.wifiLabel, wifiText.c_str());
    styleBadge(ui.wifiLabel, headerLinkFillColor(), kColorBadgeText);

    lv_label_set_text(ui.motorLabel, fitText(headerMotorLabel(), 8).c_str());
    styleBadge(ui.motorLabel, headerMotorFillColor(), kColorBadgeText);
}

void syncErrorBanner() {
    if(error == 0 || state == CALIBRATING) {
        setHidden(ui.errorBanner, true);
        return;
    }

    lv_label_set_text(ui.errorLabel, fitText(getErrorChar(error), 34).c_str());
    setHidden(ui.errorBanner, gActiveView == ActiveView::Splash);
}

void setProgress(lv_color_t color, int32_t pct, const String& label) {
    lv_bar_set_value(ui.dashProgress, clampPct(pct), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.dashProgress, color, LV_PART_INDICATOR);
    lv_label_set_text(ui.dashProgressLabel, fitText(label, 10).c_str());
}

void syncDashboardCommon(const String& mode,
                         const String& primary,
                         const String& secondary,
                         const String& hint,
                         lv_color_t accent,
                         int32_t progressPct,
                         const String& progressLabel,
                         const String& leftCaption,
                         const char* leftStatus,
                         lv_color_t leftStatusColor,
                         const String& rightCaption,
                         const char* rightStatus,
                         lv_color_t rightStatusColor) {
    lv_label_set_text(ui.dashMode, fitText(mode, 18).c_str());
    lv_obj_set_style_text_color(ui.dashMode, accent, 0);
    lv_label_set_text(ui.dashPrimary, fitText(primary, 16).c_str());
    lv_obj_set_style_text_color(ui.dashPrimary, lvColor(kColorInk), 0);
    lv_label_set_text(ui.dashSecondary, fitText(secondary, 28).c_str());
    lv_label_set_text(ui.dashHint, fitText(hint, 44).c_str());
    setHidden(ui.dashHint, hint.length() == 0);
    setProgress(accent, progressPct, progressLabel);
    lv_label_set_text(ui.dashLeftCaption, fitText(leftCaption, 6).c_str());
    lv_label_set_text(ui.dashLeftStatus, leftStatus != nullptr ? leftStatus : "");
    lv_obj_set_style_text_color(ui.dashLeftStatus, leftStatusColor, 0);
    lv_label_set_text(ui.dashRightCaption, fitText(rightCaption, 6).c_str());
    lv_label_set_text(ui.dashRightStatus, rightStatus != nullptr ? rightStatus : "");
    lv_obj_set_style_text_color(ui.dashRightStatus, rightStatusColor, 0);
}

void syncDashboard() {
    const int32_t liveRpm = currentDisplayMotorRPM();

    switch(state) {
        case IDLE: {
            syncDashboardCommon("Manual",
                                String(setRPM) + " rpm",
                                "Ready to grind",
                                "",
                                lvColor(kColorAccent),
                                percentOfRange(setRPM, Menu1[SETMIN].value, Menu1[SETMAX].value),
                                "Target",
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor(),
                                "Purge",
                                purgeStatusSymbol(),
                                purgeStatusColor());
            break;
        }
        case IDLE_GBW: {
            syncDashboardCommon("Grind by Weight",
                                formatMassMg(setWeight),
                                String("Target at ") + Menu3[GBW_RPM_SET].value + " rpm",
                                "",
                                lvColor(kColorGood),
                                percentOfRange(static_cast<int32_t>(setWeight),
                                               default_min_weight,
                                               default_max_weight),
                                "Dose",
                                "Scale",
                                scaleStatusSymbol(),
                                scaleStatusColor(),
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor());
            break;
        }
        case GRINDING: {
            const int32_t target = abs(setRPM);
            const int32_t displayRpm = abs(liveRpm);
            const int32_t pct = target > 0 ? (displayRpm * 100) / target : 0;
            const String torqueText = formatPercent(clampPct(motor_get_torque_percent()));
            syncDashboardCommon("Grinding",
                                String(displayRpm) + " rpm",
                                String("Target ") + setRPM + " rpm",
                                "",
                                lvColor(kColorRun),
                                pct,
                                formatPercent(pct),
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor(),
                                "Torque",
                                torqueText.c_str(),
                                torqueStatusColor());
            lv_obj_set_style_text_color(ui.dashPrimary, grindingRpmColor(), 0);
            break;
        }
        case PURGING: {
            const int32_t target = maxAbs(Menu2[SETPURGERPM].value, Menu2[SETPURGEREVERSE].value);
            const int32_t displayRpm = abs(liveRpm);
            const int32_t pct = target > 0 ? (displayRpm * 100) / target : 0;
            const String torqueText = formatPercent(clampPct(motor_get_torque_percent()));
            syncDashboardCommon("Purging",
                                String(displayRpm) + " rpm",
                                "Clearing the chute",
                                "",
                                lvColor(kColorWarn),
                                pct,
                                "Clear",
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor(),
                                "Torque",
                                torqueText.c_str(),
                                torqueStatusColor());
            break;
        }
        case GRINDING_GBW: {
            const int32_t pct = setWeight > 0 ? (currentWeight * 100L) / static_cast<int32_t>(setWeight) : 0;
            const String torqueText = formatPercent(clampPct(motor_get_torque_percent()));
            syncDashboardCommon("Dosing",
                                formatMassMg(currentWeight),
                                String("Target ") + formatMassMg(setWeight),
                                "",
                                lvColor(kColorGood),
                                pct,
                                formatPercent(pct),
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor(),
                                "Torque",
                                torqueText.c_str(),
                                torqueStatusColor());
            break;
        }
        case CALIBRATING: {
            const int32_t totalSteps = SIZEOFCALIBRATEARRAY / 2;
            const int32_t step = currentCal + 1;
            const int32_t pct = totalSteps > 0 ? (step * 100L) / totalSteps : 0;
            syncDashboardCommon("Calibrating",
                                String(step) + " / " + String(totalSteps),
                                "Building the grind curve",
                                "",
                                lvColor(kColorWarn),
                                pct,
                                formatPercent(pct),
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor(),
                                "Scale",
                                scaleStatusSymbol(),
                                scaleStatusColor());
            break;
        }
        default: {
            syncDashboardCommon("Controller",
                                String(setRPM) + " rpm",
                                "Ready",
                                "",
                                lvColor(kColorAccent),
                                0,
                                "",
                                "DRV",
                                driveStatusSymbol(),
                                driveStatusColor(),
                                "Purge",
                                purgeStatusSymbol(),
                                purgeStatusColor());
            break;
        }
    }
}

void syncMenuList() {
    const int menuNum = menuNumForState();
    const int rawCount = menuCountForState();
    const int selected = selectedIndexForMenuState();
    const menuEntry* menu = menuByNum(menuNum);
    if(menu == nullptr || rawCount <= 0) return;

    lv_label_set_text(ui.menuTitle, "");
    setHidden(ui.menuTitle, true);
    lv_label_set_text(ui.menuHelp, "");
    setHidden(ui.menuHelp, true);
    lv_label_set_text(ui.menuFooter, "");
    setHidden(ui.menuFooter, true);

    const int visibleCount = visibleMenuCountFor(menuNum, rawCount);
    if(visibleCount <= 0) {
        for(int row = 0; row < kVisibleMenuRows; ++row) {
            setHidden(ui.menuRows[row], true);
        }
        return;
    }

    const int selectedOrdinal = visibleOrdinalForMenuIndex(menuNum, rawCount, selected);

    for(int row = 0; row < kVisibleMenuRows; ++row) {
        const bool hero = row == 1;
        if(!hero && visibleCount <= 1) {
            setHidden(ui.menuRows[row], true);
            continue;
        }

        const int rel = row - 1;
        const int ordinal = (selectedOrdinal + rel + visibleCount) % visibleCount;
        const int itemIdx = menuIndexForVisibleOrdinal(menuNum, rawCount, ordinal);
        setHidden(ui.menuRows[row], false);

        const String label = menuItemLabel(menu[itemIdx], menuNum, itemIdx);
        lv_obj_set_style_text_font(ui.menuRowLabel[row], heroMenuFontForText(label), 0);
        lv_label_set_text(ui.menuRowLabel[row], label.c_str());
        lv_label_set_text(ui.menuRowValue[row], "");
        setHidden(ui.menuRowValue[row], true);

        if(hero) {
            lv_obj_set_style_text_font(ui.menuRowLabel[row], heroMenuFontForText(label), 0);
            stylePanel(ui.menuRows[row],
                       kColorMenuItemActive,
                       kColorMenuItemActiveLine,
                       LV_OPA_COVER);
            lv_obj_set_style_border_width(ui.menuRows[row], 3, 0);
            lv_obj_set_style_text_color(ui.menuRowLabel[row],
                                        lvColor(kColorMenuItemActiveText),
                                        0);
        } else {
            lv_obj_set_style_text_font(ui.menuRowLabel[row], &lv_font_montserrat_14, 0);
            stylePanel(ui.menuRows[row],
                       blendRgb(kColorMenuItem, kColorPanel, 0.30f),
                       blendRgb(kColorLine, kColorBg, 0.20f),
                       LV_OPA_80);
            lv_obj_set_style_border_width(ui.menuRows[row], 1, 0);
            lv_obj_set_style_text_color(ui.menuRowLabel[row],
                                        lvColor(kColorMuted),
                                        0);
        }
    }
}

void syncMenuDetail() {
    const int menuNum = menuNumForState();
    const int selected = selectedIndexForMenuState();
    const menuEntry* menu = menuByNum(menuNum);
    if(menu == nullptr) return;

    const menuEntry& item = menu[selected];
    lv_label_set_text(ui.detailTitle, "");
    setHidden(ui.detailTitle, true);
    const String value = menuDetailValue(item, menuNum, selected);
    lv_obj_set_width(ui.detailValue, 208);
    lv_obj_set_style_text_font(ui.detailValue, heroMenuFontForText(value), 0);
    lv_obj_set_style_text_align(ui.detailValue, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui.detailValue);
    lv_label_set_text(ui.detailValue, value.c_str());
    lv_obj_set_style_text_color(ui.detailValue, lvColor(kColorMenuItemActiveText), 0);
    lv_label_set_text(ui.detailMeta, "");
    setHidden(ui.detailMeta, true);
    lv_label_set_text(ui.detailHint, "");
    setHidden(ui.detailHint, true);
}

void syncNetworkInfo() {
    lv_label_set_text(ui.detailTitle, "");
    setHidden(ui.detailTitle, true);

    lv_obj_set_width(ui.detailValue, 214);
    lv_obj_set_style_text_font(ui.detailValue, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ui.detailValue, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(ui.detailValue, lvColor(kColorSoft), 0);
    lv_obj_align(ui.detailValue, LV_ALIGN_TOP_LEFT, -3, -6);
    lv_label_set_text(ui.detailValue, networkInfoText().c_str());

    lv_label_set_text(ui.detailMeta, "");
    setHidden(ui.detailMeta, true);
    lv_label_set_text(ui.detailHint, "Start / encoder: back");
    lv_obj_set_style_text_color(ui.detailHint, lvColor(kColorMuted), 0);
    setHidden(ui.detailHint, false);
}

void syncMotorSelect() {
    lv_label_set_text(ui.selectTitle, "");
    setHidden(ui.selectTitle, true);
    lv_label_set_text(ui.selectBody, "");
    setHidden(ui.selectBody, true);

    for(int row = 0; row < kVisibleMotorSelectRows; ++row) {
        const bool hero = row == 1;
        setHidden(ui.selectRows[row], !hero);
        if(!hero) continue;

        lv_label_set_text(ui.selectLabels[row], motorSelectTitle(motorSelectSelected));
        lv_label_set_text(ui.selectHints[row], "");
        setHidden(ui.selectHints[row], true);
        stylePanel(ui.selectRows[row],
                   kColorMenuItemActive,
                   kColorMenuItemActiveLine);
        lv_obj_set_style_border_width(ui.selectRows[row], 3, 0);
        lv_obj_set_style_text_color(ui.selectLabels[row],
                                    lvColor(kColorMenuItemActiveText),
                                    0);
        lv_obj_set_style_text_color(ui.selectHints[row],
                                    lvColor(kColorMenuItemActiveText),
                                    0);
    }
}

bool splashActive() {
    if(gSplashUntil == 0 || gSplashLabel.length() == 0) return false;
    if(millis() >= gSplashUntil) {
        gSplashUntil = 0;
        gSplashLabel = "";
        return false;
    }
    return true;
}

void syncViewForState() {
    applyViewPalette();

    if(splashActive()) {
        showView(ActiveView::Splash);
        lv_label_set_text(ui.splashLabel, fitText(gSplashLabel, 24).c_str());
        return;
    }

    if(state == MOTOR_SELECT) {
        showView(ActiveView::Select);
        syncMotorSelect();
        syncHeader();
        syncErrorBanner();
        return;
    }

    if(state == NETWORK_INFO) {
        showView(ActiveView::Detail);
        syncNetworkInfo();
        syncHeader();
        syncErrorBanner();
        return;
    }

    if(state == MENU1 || state == MENU2 || state == MENU3 || state == MENU4 || state == MENU5) {
        if(enterMenu) {
            showView(ActiveView::Detail);
            syncMenuDetail();
        } else {
            showView(ActiveView::Menu);
            syncMenuList();
        }
        syncHeader();
        syncErrorBanner();
        return;
    }

    showView(ActiveView::Dashboard);
    syncDashboard();
    syncHeader();
    syncErrorBanner();
}

}  // namespace

void controller_lvgl_init(Adafruit_ST7789* display, uint16_t width, uint16_t height) {
    if(gReady) {
        gDisplay = display;
        return;
    }

    if(display == nullptr || width == 0 || height == 0) return;

    gDisplay = display;
    lv_init();

    lv_disp_draw_buf_init(&gDrawBuffer,
                          gDrawPixels,
                          nullptr,
                          static_cast<uint32_t>(kScreenWidth * kDrawBufferLines));

    lv_disp_drv_init(&gDisplayDriver);
    gDisplayDriver.hor_res = width;
    gDisplayDriver.ver_res = height;
    gDisplayDriver.flush_cb = flushDisplay;
    gDisplayDriver.draw_buf = &gDrawBuffer;
    lv_disp_drv_register(&gDisplayDriver);

    createUi();
    gEnabled = true;
    gReady = true;
    gLastTickAt = millis();
    syncViewForState();
}

void controller_lvgl_sync() {
    if(!gReady || !gEnabled) return;

    const uint32_t now = millis();
    if(gLastTickAt == 0) gLastTickAt = now;
    if(now >= gLastTickAt) lv_tick_inc(now - gLastTickAt);
    gLastTickAt = now;

    syncViewForState();
    lv_timer_handler();
}

void controller_lvgl_set_enabled(bool enabled) {
    gEnabled = enabled;
    gLastTickAt = millis();
}

void controller_lvgl_set_output_enabled(bool enabled) {
    controller_lvgl_set_enabled(enabled);
}

void controller_lvgl_invalidate() {
    if(!gReady) return;
    lv_obj_invalidate(ui.root);
}

bool controller_lvgl_ready() {
    return gReady;
}

void controller_lvgl_show_motor_probe_splash(const char* motorLabel) {
    controller_lvgl_show_flash(motorLabel, kSplashVisibleMs);
}

void controller_lvgl_show_flash(const char* label, uint32_t durationMs) {
    if(label == nullptr || label[0] == '\0') return;
    gSplashLabel = label;
    gSplashUntil = millis() + (durationMs > 0 ? durationMs : kSplashVisibleMs);
}
