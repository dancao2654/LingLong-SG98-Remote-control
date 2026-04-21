#include "remote_app.h"

#include <math.h>

#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "remote_board.h"
#include "remote_config.h"
#include "remote_wifi.h"
#include "sg98_remote_defs.h"
#include <sg98_remote_protocol.h>

namespace {

constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorBg = 0x0841;
constexpr uint16_t kColorPanel = 0x10A3;
constexpr uint16_t kColorPanelHi = 0x18E5;
constexpr uint16_t kColorAccent = 0x2CF9;
constexpr uint16_t kColorAccent2 = 0xFD20;
constexpr uint16_t kColorWarn = 0xFD20;
constexpr uint16_t kColorDanger = 0xF9C6;
constexpr uint16_t kColorGood = 0x3D8A;
constexpr uint16_t kColorMuted = 0x7BEF;
constexpr uint16_t kColorText = 0xFFFF;

constexpr uint16_t kTapMoveThreshold = 10;
constexpr uint8_t kMaxVisibleRows = 6;

enum class Screen : uint8_t {
    Home = 0,
    Main = 1,
    Purge = 2,
    Gbw = 3,
    Drive = 4,
};

enum class ItemKind : uint8_t {
    Value,
    Toggle,
    Action,
    Danger,
};

enum class LinkTransport : uint8_t {
    None = 0,
    EspNow = 1,
    WebSocket = 2,
};

struct SettingItem {
    const char* label;
    ItemKind kind;
    uint8_t menuNum;
    uint8_t itemIdx;
    int32_t minValue;
    int32_t maxValue;
    int32_t step;
    const char* unit;
    const char* action;
    const char* actionArg;
};

struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;

    Rect() = default;
    Rect(int16_t rectX, int16_t rectY, int16_t rectW, int16_t rectH)
        : x(rectX), y(rectY), w(rectW), h(rectH) {}

    bool contains(int16_t px, int16_t py) const {
        return px >= x && py >= y && px < (x + w) && py < (y + h);
    }
};

struct Layout {
    int16_t width = 0;
    int16_t height = 0;
    int16_t pad = 0;
    int16_t gap = 0;
    int16_t topPillH = 0;
    int16_t buttonH = 0;
    int16_t navH = 0;
    int16_t rowH = 0;
    uint8_t visibleRows = 0;
    int16_t listTop = 0;
    int16_t listBottom = 0;
    int16_t footerY = 0;
    Rect tabs[5];
    Rect topLeft;
    Rect topCenter;
    Rect topRight;
    Rect modeBadge;
    Rect homeModeButton;
    Rect homeActionButton;
    Rect menuRows[kMaxVisibleRows];
    Rect footer;
    Rect editCancel;
    Rect editSave;
    int16_t homeDialCx = 0;
    int16_t homeDialCy = 0;
    int16_t homeDialOuterRadius = 0;
    int16_t homeDialInnerRadius = 0;
    int16_t editorCx = 0;
    int16_t editorCy = 0;
    int16_t editorOuterRadius = 0;
    int16_t editorInnerRadius = 0;
    int firstRow = 0;
    uint8_t rowCount = 0;
};

enum class TouchMode : uint8_t {
    Tap,
    HomeDial,
    EditorDial,
    MenuScroll,
};

struct TouchTracker {
    bool active = false;
    bool dragUsed = false;
    TouchMode mode = TouchMode::Tap;
    int16_t startX = 0;
    int16_t startY = 0;
    int16_t lastX = 0;
    int16_t lastY = 0;
    int16_t scrollAccum = 0;
    float lastAngle = 0.0f;
    float angleAccum = 0.0f;
    uint32_t startedAt = 0;
};

struct AppState {
    Screen screen = Screen::Home;
    uint8_t selection[5] = {};
    bool editing = false;
    bool targetDirty = false;
    int32_t draftValue = 0;
    int32_t manualRpm = default_setRPM;
    int32_t weightMg = default_setWeight;
    int32_t menuValues[5][sg98_protocol::kMaxMenuValues] = {};
    bool menuSynced[5] = {};
    sg98_protocol::StatusFrame status;
    bool hasStatus = false;
    bool preferGbw = false;
    bool redraw = true;
    bool radioModeKnown = false;
    bool espNowReady = false;
    bool wsConnected = false;
    bool wsClientStarted = false;
    bool mdnsReady = false;
    bool sendComplete = false;
    bool sendOk = false;
    LinkTransport currentTransport = LinkTransport::None;
    LinkTransport pendingTransport = LinkTransport::None;
    uint32_t targetCommitAt = 0;
    uint32_t pendingTransportAt = 0;
    uint32_t lastTransportSwitchAt = 0;
    uint32_t toastUntil = 0;
    uint32_t lastStatusAt = 0;
    uint32_t lastStatusPollAt = 0;
    uint32_t lastBackgroundMenuPollAt = 0;
    uint32_t lastMdnsQueryAt = 0;
    uint32_t lastMenuPollAt[5] = {};
    uint32_t dangerArmUntil = 0;
    Screen dangerScreen = Screen::Home;
    uint8_t dangerSelection = 0;
    uint32_t lastUserActivityAt = 0;
    uint32_t lastBatteryPollAt = 0;
    RemoteBatteryStatus battery;
    String wsHost;
    String wsEndpointLabel;
    String mdnsHostName;
    uint16_t wsPort = remote_config::kWebSocketPort;
    String toast = "Booting";
};

template <size_t N>
constexpr size_t arrayCount(const SettingItem (&)[N]) {
    return N;
}

static_assert(NUM_MENU1_ITEMS <= sg98_protocol::kMaxMenuValues, "menu1 frame too small");
static_assert(NUM_MENU2_ITEMS <= sg98_protocol::kMaxMenuValues, "menu2 frame too small");
static_assert(NUM_MENU3_ITEMS <= sg98_protocol::kMaxMenuValues, "menu3 frame too small");
static_assert(NUM_MENU4_ITEMS <= sg98_protocol::kMaxMenuValues, "menu4 frame too small");

constexpr SettingItem kMainItems[] = {
    {"Radio mode", ItemKind::Toggle, 1, RADIOMODE, 0, 1, 1, "", nullptr, nullptr},
    {"Sleep time", ItemKind::Value, 1, SETSLEEP, 0, 300, 5, "min", nullptr, nullptr},
    {"Brightness", ItemKind::Value, 1, DISP_BRIGHTNESS, 1, 255, 1, "", nullptr, nullptr},
    {"LED max", ItemKind::Value, 1, LED_BRIGHTNESS, 0, 100, 1, "%", nullptr, nullptr},
    {"Invert scroll", ItemKind::Toggle, 1, INVERT_SCROLL, 0, 1, 1, "", nullptr, nullptr},
    {"Max RPM", ItemKind::Value, 1, SETMAX, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Min RPM", ItemKind::Value, 1, SETMIN, absolute_min_rpm, 500, rpm_scalar, "rpm", nullptr, nullptr},
    {"Motor torque", ItemKind::Value, 1, SETMOTORTORQUE, 1, 300, 1, "%", nullptr, nullptr},
    {"Motor ramp", ItemKind::Value, 1, SETMOTORRAMP, 0, 12, 1, "lvl", nullptr, nullptr},
    {"Calibrate", ItemKind::Action, 0, 0, 0, 0, 0, "", "calibrate", nullptr},
    {"Reset network", ItemKind::Action, 0, 0, 0, 0, 0, "", "reset_network", nullptr},
    {"Factory reset", ItemKind::Danger, 0, 0, 0, 0, 0, "", "reset_defaults", "confirm"},
};

constexpr SettingItem kPurgeItems[] = {
    {"Auto purge", ItemKind::Toggle, 2, AUTO_PURGE_ENABLED, 0, 1, 1, "", nullptr, nullptr},
    {"Button purge", ItemKind::Toggle, 2, SETBUTTONPURGE, 0, 1, 1, "", nullptr, nullptr},
    {"Forward RPM", ItemKind::Value, 2, SETPURGERPM, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Reverse RPM", ItemKind::Value, 2, SETPURGEREVERSE, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Delay", ItemKind::Value, 2, SETPURGEDELAY, 100, 10000, 50, "ms", nullptr, nullptr},
    {"Duration", ItemKind::Value, 2, SETPURGETIME, 500, 5000, 100, "ms", nullptr, nullptr},
    {"Zero RPM time", ItemKind::Value, 2, SETPURGEOFFTIME, 0, 5000, 10, "ms", nullptr, nullptr},
    {"Stabilize", ItemKind::Value, 2, SETPURGESTABILTIME, 100, 5000, 50, "ms", nullptr, nullptr},
    {"Frames high", ItemKind::Value, 2, SETPURGEFRAMESHIGH, 2, 20, 1, "", nullptr, nullptr},
    {"Frames low", ItemKind::Value, 2, SETPURGEFRAMESLOW, 2, 20, 1, "", nullptr, nullptr},
    {"Scalar high", ItemKind::Value, 2, SETPURGEPRCTHIGH, 101, 200, 1, "%", nullptr, nullptr},
    {"Scalar low", ItemKind::Value, 2, SETPURGEPRCTLOW, 101, 200, 1, "%", nullptr, nullptr},
    {"Auto off", ItemKind::Value, 2, AUTO_OFF_TIME, 0, 300, 10, "s", nullptr, nullptr},
};

constexpr SettingItem kGbwItems[] = {
    {"GbW RPM", ItemKind::Value, 3, GBW_RPM_SET, 200, 1500, rpm_scalar, "rpm", nullptr, nullptr},
    {"Slow RPM", ItemKind::Value, 3, GBW_SLOW_RPM, rpm_scalar, 500, rpm_scalar, "rpm", nullptr, nullptr},
    {"Slow dose", ItemKind::Value, 3, GBW_SLOW_MG, 0, 6000, 100, "mg", nullptr, nullptr},
    {"Start delay", ItemKind::Value, 3, GBW_BUTTON_DELAY, 0, 1000, 50, "ms", nullptr, nullptr},
    {"Speed mod", ItemKind::Value, 3, GBW_SPEEDMOD, 1, 32767, 100, "", nullptr, nullptr},
    {"Time offset", ItemKind::Value, 3, GBW_OFFSET, 10, 1000, 10, "ms", nullptr, nullptr},
    {"Save scale", ItemKind::Toggle, 3, SAVE_SCALE, 0, 1, 1, "", nullptr, nullptr},
};

constexpr SettingItem kDriveItems[] = {
    {"Auto detect", ItemKind::Action, 0, 0, 0, 0, 0, "", "redetect", nullptr},
    {"Force JMC", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "jmc"},
    {"Force RT", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "rt"},
    {"JMC 9600", ItemKind::Action, 0, 0, 0, 0, 0, "", "jmc_baud", "9600"},
    {"JMC 19200", ItemKind::Action, 0, 0, 0, 0, 0, "", "jmc_baud", "19200"},
    {"JMC 38400", ItemKind::Action, 0, 0, 0, 0, 0, "", "jmc_baud", "38400"},
    {"JMC 57600", ItemKind::Action, 0, 0, 0, 0, 0, "", "jmc_baud", "57600"},
    {"JMC 115200", ItemKind::Action, 0, 0, 0, 0, 0, "", "jmc_baud", "115200"},
};

constexpr Screen kTabScreens[] = {
    Screen::Home,
    Screen::Main,
    Screen::Purge,
    Screen::Gbw,
    Screen::Drive,
};

constexpr const char* kTabLabels[] = {"RUN", "MAIN", "PURGE", "GBW", "DRIVE"};

AppState app;
TouchTracker touchTracker;
WebSocketsClient wsClient;

const char* transportName(LinkTransport transport) {
    switch(transport) {
        case LinkTransport::EspNow: return "ESP-NOW";
        case LinkTransport::WebSocket: return "WebSocket";
        default: return "None";
    }
}

LinkTransport transportFromRadioMode(int32_t radioMode) {
    return radioMode == 0 ? LinkTransport::WebSocket : LinkTransport::EspNow;
}

bool prefersWebSocket() {
    return (app.radioModeKnown && transportFromRadioMode(app.menuValues[1][RADIOMODE]) == LinkTransport::WebSocket) ||
           app.pendingTransport == LinkTransport::WebSocket;
}

bool webSocketCandidateAvailable() {
    return remote_wifi_has_saved_network() || prefersWebSocket();
}

LinkTransport initialTransport() {
    if(remote_wifi_has_saved_network()) {
        return LinkTransport::WebSocket;
    }
    if(remote_config::kTargetMacConfigured) {
        return LinkTransport::EspNow;
    }
    return LinkTransport::WebSocket;
}

int16_t clampI16(int16_t value, int16_t minValue, int16_t maxValue) {
    if(value < minValue) return minValue;
    if(value > maxValue) return maxValue;
    return value;
}

int32_t clampValue(int32_t value, int32_t minValue, int32_t maxValue) {
    if(value < minValue) return minValue;
    if(value > maxValue) return maxValue;
    return value;
}

bool isLargeCanvas() {
    return remote_board_info().width >= 380;
}

RemoteTextStyle heroTextStyle() {
    return isLargeCanvas() ? RemoteTextStyle::Hero : RemoteTextStyle::Large;
}

RemoteTextStyle bodyTextStyle() {
    return isLargeCanvas() ? RemoteTextStyle::Medium : RemoteTextStyle::Small;
}

RemoteTextStyle tinyTextStyle() {
    return isLargeCanvas() ? RemoteTextStyle::Small : RemoteTextStyle::Tiny;
}

String weightLabel(int32_t mg) {
    String out;
    const int32_t whole = mg / 1000;
    const int32_t frac = abs(mg % 1000);
    out.reserve(16);
    out += String(whole);
    out += '.';
    if(frac < 100) out += '0';
    if(frac < 10) out += '0';
    out += String(frac);
    out += " g";
    return out;
}

bool batteryStatusEqual(const RemoteBatteryStatus& lhs, const RemoteBatteryStatus& rhs) {
    return lhs.supported == rhs.supported &&
           lhs.batteryPresent == rhs.batteryPresent &&
           lhs.usbPresent == rhs.usbPresent &&
           lhs.charging == rhs.charging &&
           lhs.discharging == rhs.discharging &&
           lhs.standby == rhs.standby &&
           lhs.percent == rhs.percent &&
           lhs.millivolts == rhs.millivolts &&
           lhs.vbusMillivolts == rhs.vbusMillivolts &&
           lhs.systemMillivolts == rhs.systemMillivolts;
}

String voltageLabel(int32_t millivolts) {
    if(millivolts <= 0) {
        return "-";
    }

    const int32_t centivolts = (millivolts + 5) / 10;
    const int32_t whole = centivolts / 100;
    const int32_t frac = abs(centivolts % 100);

    String out;
    out.reserve(8);
    out += String(whole);
    out += '.';
    if(frac < 10) out += '0';
    out += String(frac);
    out += 'V';
    return out;
}

bool isLinkOnline() {
    return app.hasStatus && (millis() - app.lastStatusAt) < remote_config::kLinkTimeoutMs;
}

bool isGbwState() {
    if(app.hasStatus) {
        return app.status.state == "IDLE_GBW" || app.status.state == "GRINDING_GBW";
    }
    return app.preferGbw;
}

bool isRunningState() {
    return app.hasStatus &&
           (app.status.state == "GRINDING" || app.status.state == "GRINDING_GBW" || app.status.state == "PURGING");
}

String batteryPillLabel(const String& fallback) {
    const RemoteBatteryStatus& battery = app.battery;
    if(!battery.supported) {
        return fallback;
    }

    if(battery.charging) {
        if(battery.percent >= 0) {
            return String("CHG ") + String(static_cast<int>(battery.percent)) + "%";
        }
        return "CHG";
    }

    if(battery.usbPresent && !battery.batteryPresent) {
        return "USB IN";
    }

    if(battery.usbPresent) {
        if(battery.percent >= 0) {
            return String("USB ") + String(static_cast<int>(battery.percent)) + "%";
        }
        return "USB";
    }

    if(battery.percent >= 0) {
        return String("BAT ") + String(static_cast<int>(battery.percent)) + "%";
    }

    if(battery.millivolts > 0) {
        return String("BAT ") + voltageLabel(battery.millivolts);
    }

    return "BAT --";
}

uint16_t batteryPillFill(uint16_t fallbackFill) {
    const RemoteBatteryStatus& battery = app.battery;
    if(!battery.supported) {
        return fallbackFill;
    }
    if(battery.charging) {
        return kColorGood;
    }
    if(battery.usbPresent) {
        return kColorAccent;
    }
    if(battery.percent >= 50) {
        return kColorGood;
    }
    if(battery.percent >= 20) {
        return kColorWarn;
    }
    if(battery.percent >= 0) {
        return kColorDanger;
    }
    return fallbackFill;
}

uint16_t batteryPillTextColor(uint16_t fallbackTextColor) {
    return app.battery.supported ? kColorBlack : fallbackTextColor;
}

uint8_t currentBacklightLevel() {
    const int32_t configured = app.menuValues[1][DISP_BRIGHTNESS];
    if(configured > 0) {
        return static_cast<uint8_t>(clampValue(configured, 1, 255));
    }
    return static_cast<uint8_t>(default_brightness);
}

void markUserActivity() {
    app.lastUserActivityAt = millis();
}

bool stateAllowsModeToggle() {
    return !app.hasStatus || app.status.state == "IDLE" || app.status.state == "IDLE_GBW";
}

void setToast(const String& text, uint32_t durationMs = remote_config::kToastHoldMs) {
    app.toast = text;
    app.toastUntil = millis() + durationMs;
    app.redraw = true;
}

void refreshBatteryStatus(bool force) {
    const uint32_t now = millis();
    if(!force && (now - app.lastBatteryPollAt) < remote_config::kBatteryPollMs) {
        return;
    }

    app.lastBatteryPollAt = now;
    const RemoteBatteryStatus latest = remote_board_battery_status();
    if(force || !batteryStatusEqual(latest, app.battery)) {
        app.battery = latest;
        app.redraw = true;
    }
}

const SettingItem* itemsForScreen(Screen screen, size_t* count) {
    switch(screen) {
        case Screen::Main:
            if(count != nullptr) *count = arrayCount(kMainItems);
            return kMainItems;
        case Screen::Purge:
            if(count != nullptr) *count = arrayCount(kPurgeItems);
            return kPurgeItems;
        case Screen::Gbw:
            if(count != nullptr) *count = arrayCount(kGbwItems);
            return kGbwItems;
        case Screen::Drive:
            if(count != nullptr) *count = arrayCount(kDriveItems);
            return kDriveItems;
        case Screen::Home:
        default:
            if(count != nullptr) *count = 0;
            return nullptr;
    }
}

uint8_t& selectionFor(Screen screen) {
    return app.selection[static_cast<uint8_t>(screen)];
}

int32_t menuValue(const SettingItem& item) {
    if(item.menuNum == 0) {
        return 0;
    }
    return app.menuValues[item.menuNum][item.itemIdx];
}

String formatValueLabel(const SettingItem& item, int32_t value) {
    if(item.kind == ItemKind::Toggle) {
        return value != 0 ? "ON" : "OFF";
    }

    if(item.itemIdx == GBW_SLOW_MG) {
        return weightLabel(value);
    }

    String out = String(value);
    if(item.unit != nullptr && item.unit[0] != '\0') {
        out += ' ';
        out += item.unit;
    }
    return out;
}

String formatSettingValue(const SettingItem& item) {
    if(item.kind == ItemKind::Action || item.kind == ItemKind::Danger) {
        if(item.action != nullptr && strcmp(item.action, "drive") == 0) {
            if(app.hasStatus) {
                if((item.actionArg != nullptr && app.status.drive == "JMC" && strcmp(item.actionArg, "jmc") == 0) ||
                   (item.actionArg != nullptr && app.status.drive == "RT" && strcmp(item.actionArg, "rt") == 0)) {
                    return "ACTIVE";
                }
            }
            return "RUN";
        }
        if(item.action != nullptr && strcmp(item.action, "jmc_baud") == 0) {
            if(app.hasStatus && item.actionArg != nullptr && app.status.jmcBaud == atoi(item.actionArg)) {
                return "ACTIVE";
            }
            return item.actionArg != nullptr ? item.actionArg : "RUN";
        }
        return item.kind == ItemKind::Danger ? "HOLD" : "RUN";
    }

    return formatValueLabel(item, menuValue(item));
}

bool dangerIsArmed(Screen screen, uint8_t selectedIdx) {
    return app.dangerArmUntil > millis() &&
           app.dangerScreen == screen &&
           app.dangerSelection == selectedIdx;
}

void armDanger(Screen screen, uint8_t selectedIdx) {
    app.dangerScreen = screen;
    app.dangerSelection = selectedIdx;
    app.dangerArmUntil = millis() + remote_config::kDangerConfirmMs;
}

void seedMenuDefaults() {
    memset(app.menuValues, 0, sizeof(app.menuValues));

    app.menuValues[1][RADIOMODE] = default_radio_mode;
    app.menuValues[1][SETSLEEP] = default_sleepTime;
    app.menuValues[1][DISP_BRIGHTNESS] = default_brightness;
    app.menuValues[1][LED_BRIGHTNESS] = default_led_brightness;
    app.menuValues[1][INVERT_SCROLL] = 0;
    app.menuValues[1][SETMAX] = default_maxRPM;
    app.menuValues[1][SETMIN] = default_minRPM;
    app.menuValues[1][SETMOTORTORQUE] = default_motor_torque;
    app.menuValues[1][SETMOTORRAMP] = default_motor_ramp;

    app.menuValues[2][AUTO_PURGE_ENABLED] = default_autoPurgeEnabled;
    app.menuValues[2][SETBUTTONPURGE] = default_buttonPurge;
    app.menuValues[2][SETPURGERPM] = default_purgeForwardRPM;
    app.menuValues[2][SETPURGEREVERSE] = default_reverseRotation;
    app.menuValues[2][SETPURGEDELAY] = default_purgeDelay;
    app.menuValues[2][SETPURGETIME] = default_purgeDuration;
    app.menuValues[2][SETPURGEOFFTIME] = default_purge_off_time;
    app.menuValues[2][SETPURGESTABILTIME] = default_purgeStabilTime;
    app.menuValues[2][SETPURGEFRAMESHIGH] = default_purgeFramesHigh;
    app.menuValues[2][SETPURGEFRAMESLOW] = default_purgeFramesLow;
    app.menuValues[2][SETPURGEPRCTHIGH] = default_purgePrctHigh;
    app.menuValues[2][SETPURGEPRCTLOW] = default_purgePrctLow;
    app.menuValues[2][AUTO_OFF_TIME] = default_auto_off_time;

    app.menuValues[3][GBW_RPM_SET] = default_GBWRPM;
    app.menuValues[3][GBW_SLOW_RPM] = default_slow_rpm;
    app.menuValues[3][GBW_SLOW_MG] = default_slow_mg;
    app.menuValues[3][GBW_BUTTON_DELAY] = default_start_delay;
    app.menuValues[3][GBW_SPEEDMOD] = default_speedModifier;
    app.menuValues[3][GBW_OFFSET] = default_time_offset;
    app.menuValues[3][SAVE_SCALE] = 0;
}

RemoteIndicatorState currentIndicator() {
    RemoteIndicatorState indicator = {};
    indicator.linkOnline = isLinkOnline();
    indicator.running = isRunningState();
    indicator.gbwMode = isGbwState();
    indicator.hasError = app.hasStatus && app.status.error != 0;
    return indicator;
}

Layout buildLayout() {
    Layout layout = {};
    layout.width = remote_board_canvas().width();
    layout.height = remote_board_canvas().height();

    const int16_t shortSide = layout.width < layout.height ? layout.width : layout.height;
    layout.pad = clampI16(shortSide / 24, 10, 20);
    layout.gap = clampI16(shortSide / 40, 6, 12);
    layout.topPillH = clampI16(shortSide / 13, 22, 34);
    layout.buttonH = clampI16(shortSide / 10, 24, 42);
    layout.navH = clampI16(shortSide / 10, 24, 44);

    const int16_t tabY = layout.height - layout.pad - layout.navH;
    const int16_t tabGap = layout.gap;
    const int16_t tabW = static_cast<int16_t>((layout.width - (layout.pad * 2) - (tabGap * 4)) / 5);
    for(uint8_t i = 0; i < 5; ++i) {
        layout.tabs[i] = {
            static_cast<int16_t>(layout.pad + i * (tabW + tabGap)),
            tabY,
            tabW,
            layout.navH
        };
    }

    const int16_t pillGap = layout.gap;
    const int16_t pillW = static_cast<int16_t>((layout.width - (layout.pad * 2) - (pillGap * 2)) / 3);
    layout.topLeft = {layout.pad, layout.pad, pillW, layout.topPillH};
    layout.topCenter = {static_cast<int16_t>(layout.topLeft.x + pillW + pillGap), layout.pad, pillW, layout.topPillH};
    layout.topRight = {static_cast<int16_t>(layout.topCenter.x + pillW + pillGap), layout.pad, pillW, layout.topPillH};

    layout.homeDialCx = static_cast<int16_t>(layout.width / 2);
    layout.homeDialCy = static_cast<int16_t>((layout.height / 2) - (layout.navH / 4));
    layout.homeDialOuterRadius = clampI16(static_cast<int16_t>(shortSide * 0.27f), 60, 126);
    layout.homeDialInnerRadius = clampI16(static_cast<int16_t>(layout.homeDialOuterRadius * 0.62f), 34, 92);

    const int16_t modeW = clampI16(static_cast<int16_t>(layout.width * 0.52f), 136, 230);
    layout.modeBadge = {
        static_cast<int16_t>((layout.width - modeW) / 2),
        static_cast<int16_t>(layout.homeDialCy - layout.homeDialOuterRadius - layout.topPillH - layout.gap),
        modeW,
        static_cast<int16_t>(layout.topPillH + 2)
    };

    const int16_t buttonW = static_cast<int16_t>((layout.width - (layout.pad * 2) - layout.gap) / 2);
    const int16_t buttonY = static_cast<int16_t>(layout.homeDialCy + layout.homeDialOuterRadius - (layout.buttonH / 2));
    layout.homeModeButton = {layout.pad, buttonY, buttonW, layout.buttonH};
    layout.homeActionButton = {
        static_cast<int16_t>(layout.homeModeButton.x + buttonW + layout.gap),
        buttonY,
        buttonW,
        layout.buttonH
    };

    layout.listTop = layout.pad + layout.topPillH + layout.gap + 6;
    layout.listBottom = layout.height - layout.pad - layout.navH - layout.gap - 6;
    layout.visibleRows = isLargeCanvas() ? 5 : 4;
    const int16_t availableRows = static_cast<int16_t>(layout.listBottom - layout.listTop);
    layout.rowH = static_cast<int16_t>((availableRows - ((layout.visibleRows - 1) * layout.gap)) / layout.visibleRows);
    if(layout.rowH < 30) {
        layout.rowH = 30;
    }

    layout.footer = {
        static_cast<int16_t>(layout.pad * 2),
        static_cast<int16_t>(layout.listBottom - layout.topPillH + 4),
        static_cast<int16_t>(layout.width - (layout.pad * 4)),
        static_cast<int16_t>(layout.topPillH - 4)
    };

    layout.editorCx = static_cast<int16_t>(layout.width / 2);
    layout.editorCy = static_cast<int16_t>((layout.height / 2) - (layout.navH / 8));
    layout.editorOuterRadius = clampI16(static_cast<int16_t>(shortSide * 0.29f), 68, 136);
    layout.editorInnerRadius = clampI16(static_cast<int16_t>(layout.editorOuterRadius * 0.63f), 38, 96);
    layout.editCancel = {
        layout.pad,
        static_cast<int16_t>(layout.height - layout.pad - layout.navH - layout.buttonH - layout.gap),
        buttonW,
        layout.buttonH
    };
    layout.editSave = {
        static_cast<int16_t>(layout.editCancel.x + buttonW + layout.gap),
        layout.editCancel.y,
        buttonW,
        layout.buttonH
    };

    size_t count = 0;
    itemsForScreen(app.screen, &count);
    if(count > 0 && !app.editing) {
        const uint8_t selected = selectionFor(app.screen);
        layout.firstRow = 0;
        if(selected >= layout.visibleRows) {
            layout.firstRow = selected - (layout.visibleRows - 1);
        }
        if(layout.firstRow + layout.visibleRows > static_cast<int>(count)) {
            layout.firstRow = max(0, static_cast<int>(count) - static_cast<int>(layout.visibleRows));
        }
        layout.rowCount = static_cast<uint8_t>(count - layout.firstRow);
        if(layout.rowCount > layout.visibleRows) {
            layout.rowCount = layout.visibleRows;
        }
        for(uint8_t row = 0; row < layout.rowCount; ++row) {
            layout.menuRows[row] = {
                layout.pad,
                static_cast<int16_t>(layout.listTop + row * (layout.rowH + layout.gap)),
                static_cast<int16_t>(layout.width - (layout.pad * 2)),
                layout.rowH
            };
        }
    }

    return layout;
}

bool pointInRing(int16_t x,
                 int16_t y,
                 int16_t centerX,
                 int16_t centerY,
                 int16_t innerRadius,
                 int16_t outerRadius) {
    const int32_t dx = static_cast<int32_t>(x) - centerX;
    const int32_t dy = static_cast<int32_t>(y) - centerY;
    const int32_t distanceSquared = dx * dx + dy * dy;
    return distanceSquared >= static_cast<int32_t>(innerRadius) * innerRadius &&
           distanceSquared <= static_cast<int32_t>(outerRadius) * outerRadius;
}

float pointAngle(int16_t x, int16_t y, int16_t centerX, int16_t centerY) {
    return atan2f(static_cast<float>(y - centerY), static_cast<float>(x - centerX));
}

float normalizeAngle(float angle) {
    while(angle > PI) angle -= static_cast<float>(2.0 * PI);
    while(angle < -PI) angle += static_cast<float>(2.0 * PI);
    return angle;
}

void drawPill(RemoteCanvas& canvas,
              const Rect& rect,
              const String& text,
              uint16_t fill,
              uint16_t textColor) {
    const int16_t radius = rect.h / 2;
    canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, fill);
    canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, kColorPanelHi);
    canvas.drawCenteredText(text,
                            rect.x + rect.w / 2,
                            rect.y + rect.h / 2,
                            bodyTextStyle(),
                            textColor,
                            fill);
}

void drawButton(RemoteCanvas& canvas,
                const Rect& rect,
                const String& text,
                uint16_t fill,
                uint16_t textColor) {
    const int16_t radius = rect.h / 2;
    canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, fill);
    canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, kColorPanelHi);
    canvas.drawCenteredText(text,
                            rect.x + rect.w / 2,
                            rect.y + rect.h / 2,
                            bodyTextStyle(),
                            textColor,
                            fill);
}

void drawDialTicks(RemoteCanvas& canvas,
                   int16_t centerX,
                   int16_t centerY,
                   int16_t innerRadius,
                   int16_t outerRadius,
                   uint8_t count,
                   uint16_t color) {
    for(uint8_t i = 0; i < count; ++i) {
        const float angle = (static_cast<float>(i) / count) * static_cast<float>(2.0 * PI);
        const int16_t x0 = static_cast<int16_t>(centerX + cosf(angle) * innerRadius);
        const int16_t y0 = static_cast<int16_t>(centerY + sinf(angle) * innerRadius);
        const int16_t x1 = static_cast<int16_t>(centerX + cosf(angle) * outerRadius);
        const int16_t y1 = static_cast<int16_t>(centerY + sinf(angle) * outerRadius);
        canvas.drawLine(x0, y0, x1, y1, color);
    }
}

void drawTabs(RemoteCanvas& canvas, const Layout& layout) {
    for(uint8_t i = 0; i < 5; ++i) {
        const bool active = static_cast<uint8_t>(app.screen) == i;
        const uint16_t fill = active ? kColorAccent : kColorPanel;
        const uint16_t textColor = active ? kColorBlack : kColorMuted;
        drawButton(canvas, layout.tabs[i], kTabLabels[i], fill, textColor);
    }
}

void drawFooter(RemoteCanvas& canvas, const Layout& layout, const String& text) {
    drawPill(canvas, layout.footer, text, kColorPanel, kColorMuted);
}

String transportStatusText() {
    if(app.toastUntil > millis()) {
        return app.toast;
    }

    if(app.currentTransport == LinkTransport::WebSocket) {
        if(app.wsConnected) {
            return app.wsEndpointLabel.length() > 0 ? app.wsEndpointLabel : String("WebSocket linked");
        }
        if(remote_wifi_sta_connected()) {
            return "Searching LAN controller";
        }
        return String("Setup ") + remote_wifi_ap_ssid();
    }

    if(app.currentTransport == LinkTransport::EspNow) {
        return "ESP-NOW remote";
    }

    return "Searching for controller";
}

void drawFooter(RemoteCanvas& canvas, const Layout& layout) {
    drawFooter(canvas, layout, transportStatusText());
}

void drawHome(RemoteCanvas& canvas, const Layout& layout) {
    canvas.fillScreen(kColorBg);

    drawPill(canvas,
             layout.topLeft,
             isLinkOnline() ? "LINK" : "OFFLINE",
             isLinkOnline() ? kColorGood : kColorDanger,
             kColorBlack);
    drawPill(canvas,
             layout.topCenter,
             app.hasStatus ? app.status.drive : "?",
             kColorPanel,
             kColorText);
    drawPill(canvas,
             layout.topRight,
             batteryPillLabel(app.hasStatus ? app.status.comm : "NO DATA"),
             batteryPillFill(kColorPanel),
             batteryPillTextColor(kColorText));

    drawPill(canvas,
             layout.modeBadge,
             isGbwState() ? "GRIND BY WEIGHT" : "MANUAL RPM",
             isGbwState() ? kColorWarn : kColorAccent,
             kColorBlack);

    drawDialTicks(canvas,
                  layout.homeDialCx,
                  layout.homeDialCy,
                  static_cast<int16_t>(layout.homeDialOuterRadius + 6),
                  static_cast<int16_t>(layout.homeDialOuterRadius + 14),
                  isLargeCanvas() ? 24 : 18,
                  kColorPanelHi);
    canvas.fillCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius, kColorPanel);
    canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius + 2, kColorAccent);
    canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius + 6, kColorPanelHi);
    canvas.fillCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialInnerRadius, kColorBg);
    canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialInnerRadius + 1, kColorPanelHi);

    canvas.drawCenteredText(isGbwState() ? weightLabel(app.weightMg) : String(app.manualRpm),
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy - (isLargeCanvas() ? 22 : 12)),
                            heroTextStyle(),
                            kColorText,
                            kColorBg);
    canvas.drawCenteredText(isGbwState() ? "Target dose" : "Set RPM",
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy + (isLargeCanvas() ? 28 : 18)),
                            bodyTextStyle(),
                            kColorMuted,
                            kColorBg);
    canvas.drawCenteredText("Drag the ring to adjust",
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy + (isLargeCanvas() ? 58 : 34)),
                            tinyTextStyle(),
                            kColorMuted,
                            kColorBg);

    drawButton(canvas,
               layout.homeModeButton,
               isGbwState() ? "MANUAL" : "GBW",
               stateAllowsModeToggle() ? kColorAccent : kColorPanel,
               stateAllowsModeToggle() ? kColorBlack : kColorMuted);
    drawButton(canvas,
               layout.homeActionButton,
               isRunningState() ? "STOP" : "START",
               isRunningState() ? kColorDanger : kColorGood,
               kColorBlack);

    const String stateLine = app.hasStatus ? app.status.state : "WAITING";
    const String telemetryLine =
        String("RPM ") + (app.hasStatus ? String(app.status.currentRpm) : "-") +
        "  WT " + (app.hasStatus ? weightLabel(app.status.currentWeightMg) : String("-"));
    const String detailLine =
        String("COMM ") + (app.hasStatus ? app.status.comm : String("NO DATA")) +
        "  TORQ " + (app.hasStatus ? String(app.status.torquePercent) : String("-")) +
        "% ERR " + (app.hasStatus ? String(app.status.error) : String("-"));

    canvas.drawCenteredText(stateLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + 10),
                            bodyTextStyle(),
                            kColorText,
                            kColorBg);
    canvas.drawCenteredText(telemetryLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + (isLargeCanvas() ? 38 : 22)),
                            tinyTextStyle(),
                            kColorMuted,
                            kColorBg);
    canvas.drawCenteredText(detailLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + (isLargeCanvas() ? 58 : 36)),
                            tinyTextStyle(),
                            kColorMuted,
                            kColorBg);

    drawTabs(canvas, layout);
}

void drawMenuList(RemoteCanvas& canvas, const Layout& layout, const char* title) {
    canvas.fillScreen(kColorBg);

    drawPill(canvas, layout.topLeft, title, kColorAccent, kColorBlack);
    drawPill(canvas,
             layout.topCenter,
             app.hasStatus ? app.status.state : "SYNCING",
             kColorPanel,
             kColorText);
    drawPill(canvas,
             layout.topRight,
             batteryPillLabel(app.hasStatus ? app.status.drive : remote_board_info().name),
             batteryPillFill(kColorPanel),
             batteryPillTextColor(kColorText));

    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    const uint8_t selected = selectionFor(app.screen);
    for(uint8_t row = 0; row < layout.rowCount; ++row) {
        const int idx = layout.firstRow + row;
        if(idx >= static_cast<int>(count)) {
            break;
        }

        const bool active = idx == selected;
        const bool dangerArmed = items[idx].kind == ItemKind::Danger && dangerIsArmed(app.screen, selected);
        const uint16_t fill = dangerArmed ? kColorDanger : (active ? kColorPanelHi : kColorPanel);
        const uint16_t border = active ? (isLinkOnline() ? kColorAccent : kColorDanger) : kColorPanelHi;
        const uint16_t textColor = active ? kColorText : kColorMuted;
        canvas.fillRoundRect(layout.menuRows[row].x,
                             layout.menuRows[row].y,
                             layout.menuRows[row].w,
                             layout.menuRows[row].h,
                             layout.menuRows[row].h / 3,
                             fill);
        canvas.drawRoundRect(layout.menuRows[row].x,
                             layout.menuRows[row].y,
                             layout.menuRows[row].w,
                             layout.menuRows[row].h,
                             layout.menuRows[row].h / 3,
                             border);
        canvas.drawTextTopLeft(items[idx].label,
                               static_cast<int16_t>(layout.menuRows[row].x + layout.gap + 4),
                               static_cast<int16_t>(layout.menuRows[row].y + layout.gap),
                               bodyTextStyle(),
                               textColor,
                               fill);
        canvas.drawTextTopRight(formatSettingValue(items[idx]),
                                static_cast<int16_t>(layout.menuRows[row].x + layout.menuRows[row].w - layout.gap - 4),
                                static_cast<int16_t>(layout.menuRows[row].y + layout.gap),
                                bodyTextStyle(),
                                textColor,
                                fill);
    }

    const String footerText = (app.toastUntil > millis())
                                  ? app.toast
                                  : (remote_board_info().hasEncoder
                                         ? "Tap to select, encoder to browse"
                                         : "Tap rows, swipe list to browse");
    drawFooter(canvas, layout, footerText);
    drawTabs(canvas, layout);
}

void drawEditor(RemoteCanvas& canvas, const Layout& layout, const char* title) {
    canvas.fillScreen(kColorBg);

    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    const SettingItem& item = items[selectionFor(app.screen)];

    drawPill(canvas, layout.topLeft, "EDIT", kColorAccent2, kColorBlack);
    drawPill(canvas, layout.topCenter, title, kColorPanel, kColorText);
    drawPill(canvas,
             layout.topRight,
             batteryPillLabel(app.hasStatus ? app.status.state : "OFFLINE"),
             batteryPillFill(kColorPanel),
             batteryPillTextColor(kColorText));

    drawDialTicks(canvas,
                  layout.editorCx,
                  layout.editorCy,
                  static_cast<int16_t>(layout.editorOuterRadius + 8),
                  static_cast<int16_t>(layout.editorOuterRadius + 20),
                  isLargeCanvas() ? 28 : 20,
                  kColorPanelHi);
    canvas.fillCircle(layout.editorCx, layout.editorCy, layout.editorOuterRadius, kColorPanel);
    canvas.drawCircle(layout.editorCx, layout.editorCy, layout.editorOuterRadius + 3, kColorAccent2);
    canvas.drawCircle(layout.editorCx, layout.editorCy, layout.editorOuterRadius + 8, kColorPanelHi);
    canvas.fillCircle(layout.editorCx, layout.editorCy, layout.editorInnerRadius, kColorBg);
    canvas.drawCircle(layout.editorCx, layout.editorCy, layout.editorInnerRadius + 1, kColorPanelHi);

    canvas.drawCenteredText(item.label,
                            layout.editorCx,
                            static_cast<int16_t>(layout.editorCy - (isLargeCanvas() ? 40 : 26)),
                            bodyTextStyle(),
                            kColorMuted,
                            kColorBg);
    canvas.drawCenteredText(formatValueLabel(item, app.draftValue),
                            layout.editorCx,
                            layout.editorCy,
                            heroTextStyle(),
                            kColorText,
                            kColorBg);
    canvas.drawCenteredText("Drag the ring to adjust",
                            layout.editorCx,
                            static_cast<int16_t>(layout.editorCy + (isLargeCanvas() ? 44 : 28)),
                            bodyTextStyle(),
                            kColorMuted,
                            kColorBg);

    drawButton(canvas, layout.editCancel, "CANCEL", kColorPanel, kColorMuted);
    drawButton(canvas, layout.editSave, "SAVE", kColorGood, kColorBlack);
    drawTabs(canvas, layout);
}

void redrawUi() {
    RemoteCanvas& canvas = remote_board_canvas();
    const Layout layout = buildLayout();

    if(app.screen == Screen::Home) {
        drawHome(canvas, layout);
    } else if(app.editing) {
        if(app.screen == Screen::Main) {
            drawEditor(canvas, layout, "SYSTEM");
        } else if(app.screen == Screen::Purge) {
            drawEditor(canvas, layout, "PURGE");
        } else if(app.screen == Screen::Gbw) {
            drawEditor(canvas, layout, "GBW");
        } else {
            drawEditor(canvas, layout, "DRIVE");
        }
    } else if(app.screen == Screen::Main) {
        drawMenuList(canvas, layout, "SYSTEM");
    } else if(app.screen == Screen::Purge) {
        drawMenuList(canvas, layout, "PURGE");
    } else if(app.screen == Screen::Gbw) {
        drawMenuList(canvas, layout, "GBW");
    } else {
        drawMenuList(canvas, layout, "DRIVE");
    }

    app.redraw = false;
}

bool initEspNow();
void stopEspNowSession();

void clearPollSchedule() {
    app.lastStatusPollAt = 0;
    app.lastBackgroundMenuPollAt = 0;
    for(size_t i = 0; i < (sizeof(app.lastMenuPollAt) / sizeof(app.lastMenuPollAt[0])); ++i) {
        app.lastMenuPollAt[i] = 0;
    }
}

String buildRemoteMdnsHostName() {
    const uint64_t chipId = ESP.getEfuseMac();
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06llX", static_cast<unsigned long long>(chipId & 0xFFFFFFULL));
    return String("sg98-remote-") + suffix;
}

void stopMdnsClient() {
    if(!app.mdnsReady) {
        return;
    }

    MDNS.end();
    app.mdnsReady = false;
    Serial.println("[remote_ws] mdns stopped");
}

void stopWebSocketSession() {
    if(app.wsClientStarted || app.wsConnected) {
        wsClient.disconnect();
    }
    app.wsConnected = false;
    app.wsClientStarted = false;
    app.wsHost = "";
    app.wsEndpointLabel = "";
    stopMdnsClient();
}

void scheduleTransportSwitch(LinkTransport transport, uint32_t delayMs = 0) {
    app.pendingTransport = transport;
    app.pendingTransportAt = millis() + delayMs;
}

void ensureMdnsClientReady() {
    if(app.mdnsReady || !remote_wifi_sta_connected()) {
        return;
    }

    if(app.mdnsHostName.isEmpty()) {
        app.mdnsHostName = buildRemoteMdnsHostName();
    }

    if(MDNS.begin(app.mdnsHostName.c_str())) {
        app.mdnsReady = true;
        Serial.printf("[remote_ws] mdns=%s.local\n", app.mdnsHostName.c_str());
    } else {
        Serial.println("[remote_ws] mdns init failed");
    }
}

void connectWebSocketHost(const String& host, uint16_t port) {
    if(host.isEmpty()) {
        return;
    }
    if(app.wsClientStarted && app.wsHost == host && app.wsPort == port) {
        return;
    }

    wsClient.disconnect();
    app.wsConnected = false;
    app.wsHost = host;
    app.wsPort = port;
    app.wsEndpointLabel = String("WS ") + host;
    wsClient.begin(app.wsHost.c_str(), app.wsPort, "/");
    wsClient.setReconnectInterval(remote_config::kWebSocketReconnectMs);
    app.wsClientStarted = true;
    Serial.printf("[remote_ws] connect %s:%u\n",
                  app.wsHost.c_str(),
                  static_cast<unsigned>(app.wsPort));
}

void discoverControllerService(bool force) {
    if(!remote_wifi_sta_connected()) {
        return;
    }
    ensureMdnsClientReady();
    if(!app.mdnsReady) {
        return;
    }
    if(!force && (app.lastMdnsQueryAt + remote_config::kMdnsQueryMs) > millis()) {
        return;
    }

    app.lastMdnsQueryAt = millis();
    const int serviceCount = MDNS.queryService("sg98", "tcp");
    if(serviceCount <= 0) {
        return;
    }

    for(int i = 0; i < serviceCount; ++i) {
        const IPAddress ip = MDNS.IP(i);
        const uint16_t port = MDNS.port(i);
        if(ip[0] == 0 || port == 0) {
            continue;
        }
        connectWebSocketHost(ip.toString(), port);
        return;
    }
}

bool sendEspNowRaw(const String& command) {
    if(!app.espNowReady) {
        setToast(remote_config::kTargetMacConfigured ? "ESP-NOW offline" : "Set target MAC");
        return false;
    }

    app.sendComplete = false;
    app.sendOk = false;
    const esp_err_t rc = esp_now_send(remote_config::kTargetMac,
                                      reinterpret_cast<const uint8_t*>(command.c_str()),
                                      command.length() + 1);
    if(rc != ESP_OK) {
        setToast(String("TX error ") + String(static_cast<int>(rc)));
        return false;
    }
    return true;
}

bool sendWebSocketRaw(const String& command) {
    if(!app.wsConnected) {
        if(remote_wifi_sta_connected()) {
            setToast("WebSocket offline");
        } else {
            setToast(String("Setup ") + remote_wifi_ap_ssid());
        }
        return false;
    }

    String payload = command;
    wsClient.sendTXT(payload);
    return true;
}

bool sendRaw(const String& command) {
    switch(app.currentTransport) {
        case LinkTransport::WebSocket:
            return sendWebSocketRaw(command);
        case LinkTransport::EspNow:
            return sendEspNowRaw(command);
        default:
            setToast("Searching for controller");
            return false;
    }
}

void requestStatus() {
    sendRaw(sg98_protocol::kCmdGetStatus);
}

void requestMenu(uint8_t menuNum) {
    if(menuNum >= 1 && menuNum <= 4 && sendRaw(sg98_protocol::getMenu(menuNum))) {
        app.lastMenuPollAt[menuNum] = millis();
    }
}

void requestVisibleMenuIfNeeded(bool force) {
    uint8_t menuNum = 0;
    switch(app.screen) {
        case Screen::Main: menuNum = 1; break;
        case Screen::Purge: menuNum = 2; break;
        case Screen::Gbw: menuNum = 3; break;
        default: break;
    }

    if(menuNum == 0) {
        return;
    }

    if(force || millis() - app.lastMenuPollAt[menuNum] >= remote_config::kVisibleMenuPollMs) {
        requestMenu(menuNum);
    }
}

void sendAction(const char* actionName, const char* arg0 = nullptr) {
    sendRaw(sg98_protocol::action(actionName, arg0));
}

void switchTransportNow(LinkTransport transport) {
    if(transport == LinkTransport::None) {
        transport = initialTransport();
    }

    app.pendingTransport = LinkTransport::None;
    app.pendingTransportAt = 0;
    if(app.currentTransport == transport) {
        return;
    }

    if(transport == LinkTransport::WebSocket) {
        stopEspNowSession();
        if(!remote_wifi_is_active()) {
            remote_wifi_start();
        }
        if(remote_wifi_has_saved_network()) {
            setToast(remote_wifi_sta_connected() ? "WebSocket mode" : "Connecting Wi-Fi", 1200);
        } else {
            setToast(String("Setup ") + remote_wifi_ap_ssid(), 2200);
        }
    } else if(transport == LinkTransport::EspNow) {
        stopWebSocketSession();
        if(remote_wifi_is_active()) {
            remote_wifi_stop();
        }
        if(initEspNow()) {
            setToast("ESP-NOW mode", 1200);
        }
    }

    app.currentTransport = transport;
    app.lastTransportSwitchAt = millis();
    clearPollSchedule();
}

void applyPendingTransportSwitch() {
    if(app.pendingTransport == LinkTransport::None || millis() < app.pendingTransportAt) {
        return;
    }

    switchTransportNow(app.pendingTransport);
}

LinkTransport alternateTransport() {
    if(app.currentTransport == LinkTransport::WebSocket) {
        if(remote_wifi_has_saved_network() && remote_config::kTargetMacConfigured) {
            return LinkTransport::EspNow;
        }
        return app.currentTransport;
    }

    if(app.currentTransport == LinkTransport::EspNow) {
        if(webSocketCandidateAvailable()) {
            return LinkTransport::WebSocket;
        }
        return app.currentTransport;
    }

    return initialTransport();
}

void consumeStatusFrame(const sg98_protocol::StatusFrame& status) {
    app.status = status;
    app.hasStatus = true;
    app.lastStatusAt = millis();
    app.preferGbw = status.state == "IDLE_GBW" || status.state == "GRINDING_GBW";
    if(!app.targetDirty) {
        app.manualRpm = status.setRpm;
        app.weightMg = status.setWeightMg;
    }
    app.redraw = true;
}

void consumeMenuFrame(const sg98_protocol::MenuFrame& frame) {
    if(frame.menuNum >= 5) {
        return;
    }

    for(uint8_t i = 0; i < frame.count && i < sg98_protocol::kMaxMenuValues; ++i) {
        app.menuValues[frame.menuNum][i] = frame.values[i];
    }
    app.menuSynced[frame.menuNum] = true;

    if(frame.menuNum == 1) {
        app.radioModeKnown = true;
        const LinkTransport expected = transportFromRadioMode(app.menuValues[1][RADIOMODE]);
        if(expected != app.currentTransport) {
            scheduleTransportSwitch(expected);
        }
    }

    app.redraw = true;
}

void consumeProtocolFrame(const String& frame) {
    sg98_protocol::StatusFrame statusFrame;
    if(sg98_protocol::parseStatusFrame(frame, &statusFrame)) {
        consumeStatusFrame(statusFrame);
        return;
    }

    sg98_protocol::MenuFrame menuFrame;
    if(sg98_protocol::parseMenuFrame(frame, &menuFrame)) {
        consumeMenuFrame(menuFrame);
        return;
    }

    sg98_protocol::AckFrame ackFrame;
    if(sg98_protocol::parseAckFrame(frame, &ackFrame)) {
        if(ackFrame.ok && ackFrame.message == "ok") {
            return;
        }
        setToast(ackFrame.ok ? ackFrame.message : String("ERR: ") + ackFrame.message);
    }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            app.wsConnected = true;
            app.wsEndpointLabel = String("WS ") + app.wsHost;
            clearPollSchedule();
            requestStatus();
            requestMenu(1);
            requestMenu(2);
            requestMenu(3);
            break;
        case WStype_DISCONNECTED:
            app.wsConnected = false;
            app.wsEndpointLabel = "WebSocket offline";
            break;
        case WStype_TEXT: {
            String frame;
            frame.reserve(length);
            for(size_t i = 0; i < length; ++i) {
                frame += static_cast<char>(payload[i]);
            }
            consumeProtocolFrame(frame);
            break;
        }
        default:
            break;
    }
}

void onEspNowSent(const uint8_t*, esp_now_send_status_t status) {
    app.sendOk = (status == ESP_NOW_SEND_SUCCESS);
    app.sendComplete = true;
}

void onEspNowReceive(const uint8_t*, const uint8_t* data, int len) {
    if(data == nullptr || len <= 0) {
        return;
    }

    consumeProtocolFrame(String(reinterpret_cast<const char*>(data)));
}

bool initEspNow() {
    if(app.espNowReady) {
        return true;
    }
    if(!remote_config::kTargetMacConfigured) {
        setToast("Set target MAC", 60000);
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    if(esp_wifi_set_channel(remote_config::kEspNowChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        setToast("Channel setup failed");
        return false;
    }

    if(esp_now_init() != ESP_OK) {
        setToast("ESP-NOW init failed");
        return false;
    }

    if(esp_now_register_send_cb(onEspNowSent) != ESP_OK) {
        esp_now_deinit();
        setToast("Send callback failed");
        return false;
    }

    if(esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
        esp_now_deinit();
        setToast("Receive callback failed");
        return false;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, remote_config::kTargetMac, sizeof(remote_config::kTargetMac));
    peer.channel = remote_config::kEspNowChannel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if(!esp_now_is_peer_exist(remote_config::kTargetMac) && esp_now_add_peer(&peer) != ESP_OK) {
        esp_now_deinit();
        setToast("Add peer failed");
        return false;
    }

    app.espNowReady = true;
    return true;
}

void handleSendCompletion() {
    if(!app.sendComplete) {
        return;
    }

    app.sendComplete = false;
    if(!app.sendOk) {
        setToast("Send failed");
    }
}

void updateSyncFromStatusFallback() {
    if(app.hasStatus && !app.targetDirty) {
        app.manualRpm = app.status.setRpm;
        app.weightMg = app.status.setWeightMg;
    }
}

void manageTransport() {
    applyPendingTransportSwitch();
    if(app.currentTransport == LinkTransport::None) {
        switchTransportNow(initialTransport());
    }

    if(app.currentTransport == LinkTransport::WebSocket) {
        if(app.espNowReady) {
            stopEspNowSession();
        }
        if(!remote_wifi_is_active()) {
            remote_wifi_start();
        }
        remote_wifi_poll();
        if(remote_wifi_sta_connected()) {
            ensureMdnsClientReady();
            wsClient.loop();
            if(!app.wsConnected || app.wsHost.isEmpty()) {
                discoverControllerService(false);
            } else if(!isLinkOnline() && (app.lastMdnsQueryAt + remote_config::kMdnsQueryMs) <= millis()) {
                discoverControllerService(true);
            }
        } else {
            if(app.wsClientStarted || app.wsConnected) {
                wsClient.disconnect();
                app.wsClientStarted = false;
                app.wsConnected = false;
            }
            stopMdnsClient();
        }
    } else if(app.currentTransport == LinkTransport::EspNow) {
        stopWebSocketSession();
        if(remote_wifi_is_active()) {
            remote_wifi_stop();
        }
        initEspNow();
    }

    if(!isLinkOnline() && (millis() - app.lastTransportSwitchAt) >= remote_config::kTransportSearchWindowMs) {
        const LinkTransport fallback = alternateTransport();
        if(fallback != app.currentTransport) {
            switchTransportNow(fallback);
        } else {
            app.lastTransportSwitchAt = millis();
        }
    }
}

void syncPolling() {
    if(millis() - app.lastStatusPollAt >= remote_config::kStatusPollMs) {
        requestStatus();
        app.lastStatusPollAt = millis();
    }

    if(millis() - app.lastBackgroundMenuPollAt >= remote_config::kBackgroundMenuPollMs) {
        requestMenu(1);
        app.lastBackgroundMenuPollAt = millis();
    }

    requestVisibleMenuIfNeeded(false);
}

void applyMenuValue(const SettingItem& item, int32_t value) {
    if(item.menuNum == 0) {
        return;
    }

    app.menuValues[item.menuNum][item.itemIdx] = value;
    if(item.menuNum == 1 && item.itemIdx == RADIOMODE) {
        app.radioModeKnown = true;
        scheduleTransportSwitch(transportFromRadioMode(value), remote_config::kTransportSwitchDelayMs);
    }
    sendRaw(sg98_protocol::setMenuValue(item.menuNum, item.itemIdx, value));
    app.lastMenuPollAt[item.menuNum] = 0;
    app.lastStatusPollAt = 0;
}

void cancelEditing() {
    if(!app.editing) {
        return;
    }

    app.editing = false;
    setToast("Edit canceled", 900);
}

void activateSelectedItem() {
    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items == nullptr || count == 0) {
        return;
    }

    const uint8_t selected = selectionFor(app.screen);
    const SettingItem& item = items[selected];

    if(item.kind == ItemKind::Value) {
        if(!app.editing) {
            app.draftValue = menuValue(item);
            app.editing = true;
            setToast(String("Edit ") + item.label, 900);
        } else {
            applyMenuValue(item, clampValue(app.draftValue, item.minValue, item.maxValue));
            app.editing = false;
            setToast(String("Saved ") + item.label);
        }
        app.redraw = true;
        return;
    }

    if(item.kind == ItemKind::Toggle) {
        const int32_t nextValue = menuValue(item) == 0 ? 1 : 0;
        applyMenuValue(item, nextValue);
        setToast(String(item.label) + (nextValue != 0 ? " on" : " off"));
        app.redraw = true;
        return;
    }

    if(item.kind == ItemKind::Danger && !dangerIsArmed(app.screen, selected)) {
        armDanger(app.screen, selected);
        setToast("Press again to confirm", remote_config::kDangerConfirmMs);
        app.redraw = true;
        return;
    }

    sendAction(item.action, item.actionArg);
    app.lastStatusPollAt = 0;
    requestVisibleMenuIfNeeded(true);
    if(item.kind == ItemKind::Danger) {
        app.dangerArmUntil = 0;
    }
    setToast(String("Sent ") + item.label, 1000);
    app.redraw = true;
}

void setScreen(Screen screen) {
    if(app.screen == screen && !app.editing) {
        return;
    }

    app.screen = screen;
    app.editing = false;
    app.redraw = true;
    requestVisibleMenuIfNeeded(true);
}

void commitTargetIfDue() {
    if(!app.targetDirty || millis() < app.targetCommitAt) {
        return;
    }

    app.targetDirty = false;
    if(isGbwState()) {
        sendRaw(sg98_protocol::setWeightMg(app.weightMg));
    } else {
        sendRaw(sg98_protocol::setRpm(app.manualRpm));
    }
    app.lastStatusPollAt = 0;
}

void moveSelection(int delta) {
    if(app.screen == Screen::Home || app.editing) {
        return;
    }

    size_t count = 0;
    itemsForScreen(app.screen, &count);
    if(count == 0) {
        return;
    }

    uint8_t& selected = selectionFor(app.screen);
    int next = static_cast<int>(selected) + delta;
    while(next < 0) next += static_cast<int>(count);
    while(next >= static_cast<int>(count)) next -= static_cast<int>(count);
    selected = static_cast<uint8_t>(next);
    app.redraw = true;
}

void handleDialDelta(int delta) {
    if(delta == 0) {
        return;
    }

    if(app.screen == Screen::Home) {
        if(isGbwState()) {
            app.weightMg = clampValue(app.weightMg + delta * 100, default_min_weight, default_max_weight);
        } else {
            const int32_t minRpm = app.menuValues[1][SETMIN] > 0 ? app.menuValues[1][SETMIN] : default_minRPM;
            const int32_t maxRpm = app.menuValues[1][SETMAX] > 0 ? app.menuValues[1][SETMAX] : default_maxRPM;
            app.manualRpm = clampValue(app.manualRpm + delta * rpm_scalar, minRpm, maxRpm);
        }
        app.targetDirty = true;
        app.targetCommitAt = millis() + remote_config::kTargetCommitDelayMs;
        app.redraw = true;
        return;
    }

    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items == nullptr || count == 0) {
        return;
    }

    const SettingItem& item = items[selectionFor(app.screen)];
    if(app.editing && item.kind == ItemKind::Value) {
        app.draftValue = clampValue(app.draftValue + delta * item.step, item.minValue, item.maxValue);
        app.redraw = true;
        return;
    }

    moveSelection(delta);
}

void handlePrimaryShortPress() {
    if(app.screen == Screen::Home) {
        setScreen(Screen::Main);
        return;
    }

    activateSelectedItem();
}

void toggleModeAction() {
    if(!stateAllowsModeToggle()) {
        return;
    }

    sendAction(isGbwState() ? "idle" : "idle_gbw");
    app.preferGbw = !isGbwState();
    app.lastStatusPollAt = 0;
    setToast(app.preferGbw ? "GBW mode" : "Manual mode");
}

void stopEspNowSession() {
    if(app.espNowReady) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        app.espNowReady = false;
    }
    esp_wifi_stop();
    WiFi.mode(WIFI_OFF);
}

bool configureWakePin(int8_t gpioNum) {
    if(gpioNum < 0) {
        return false;
    }
    pinMode(gpioNum, INPUT_PULLUP);
    if(gpio_wakeup_enable(static_cast<gpio_num_t>(gpioNum), GPIO_INTR_LOW_LEVEL) != ESP_OK) {
        return false;
    }
    return true;
}

void clearWakePin(int8_t gpioNum) {
    if(gpioNum >= 0) {
        gpio_wakeup_disable(static_cast<gpio_num_t>(gpioNum));
    }
}

bool idleSleepAllowed() {
    if(app.editing || app.targetDirty || isRunningState()) {
        return false;
    }
    if(!remote_board_support_present(remote_board_feature_map().power.deepSleep)) {
        return false;
    }
    return (millis() - app.lastUserActivityAt) >= remote_config::kIdleSleepAfterMs;
}

void enterIdleSleepIfNeeded() {
    if(!idleSleepAllowed()) {
        return;
    }

    const LinkTransport resumeTransport = app.currentTransport == LinkTransport::None ? initialTransport() : app.currentTransport;
    const RemoteBoardPins& pins = remote_board_pins();
    bool wakeConfigured = false;
    wakeConfigured = configureWakePin(pins.touchInt) || wakeConfigured;
    wakeConfigured = configureWakePin(pins.primaryButton) || wakeConfigured;
    wakeConfigured = configureWakePin(pins.encoderSwitch) || wakeConfigured;
    if(!wakeConfigured) {
        return;
    }

    remote_board_set_backlight(0);
    remote_board_apply_indicator({});
    stopWebSocketSession();
    if(remote_wifi_is_active()) {
        remote_wifi_stop();
    }
    stopEspNowSession();
    app.currentTransport = LinkTransport::None;

    esp_sleep_enable_gpio_wakeup();
    Serial.flush();
    esp_light_sleep_start();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    clearWakePin(pins.touchInt);
    clearWakePin(pins.primaryButton);
    clearWakePin(pins.encoderSwitch);

    remote_board_set_backlight(currentBacklightLevel());
    switchTransportNow(resumeTransport);
    requestStatus();
    requestVisibleMenuIfNeeded(true);
    app.lastStatusPollAt = millis();
    app.lastBackgroundMenuPollAt = millis();
    refreshBatteryStatus(true);
    markUserActivity();
    setToast("Awake", remote_config::kWakeToastMs);
}

void handlePrimaryLongPress() {
    if(app.screen == Screen::Home) {
        toggleModeAction();
    }
}

void beginTouchTracking(int16_t x, int16_t y) {
    touchTracker = {};
    touchTracker.active = true;
    touchTracker.startX = x;
    touchTracker.startY = y;
    touchTracker.lastX = x;
    touchTracker.lastY = y;
    touchTracker.startedAt = millis();

    const Layout layout = buildLayout();
    if(app.editing &&
       pointInRing(x,
                   y,
                   layout.editorCx,
                   layout.editorCy,
                   layout.editorInnerRadius,
                   static_cast<int16_t>(layout.editorOuterRadius + 24))) {
        touchTracker.mode = TouchMode::EditorDial;
        touchTracker.lastAngle = pointAngle(x, y, layout.editorCx, layout.editorCy);
        return;
    }

    if(app.screen == Screen::Home &&
       pointInRing(x,
                   y,
                   layout.homeDialCx,
                   layout.homeDialCy,
                   layout.homeDialInnerRadius,
                   static_cast<int16_t>(layout.homeDialOuterRadius + 22))) {
        touchTracker.mode = TouchMode::HomeDial;
        touchTracker.lastAngle = pointAngle(x, y, layout.homeDialCx, layout.homeDialCy);
        return;
    }

    if(app.screen != Screen::Home && !app.editing) {
        touchTracker.mode = TouchMode::MenuScroll;
        return;
    }

    touchTracker.mode = TouchMode::Tap;
}

void updateDialTouch(int16_t x, int16_t y, bool editorMode) {
    const Layout layout = buildLayout();
    const int16_t centerX = editorMode ? layout.editorCx : layout.homeDialCx;
    const int16_t centerY = editorMode ? layout.editorCy : layout.homeDialCy;
    const float angle = pointAngle(x, y, centerX, centerY);
    float delta = normalizeAngle(angle - touchTracker.lastAngle);
    touchTracker.lastAngle = angle;
    touchTracker.angleAccum += delta;

    const float stepAngle = isLargeCanvas() ? 0.12f : 0.16f;
    int steps = 0;
    while(touchTracker.angleAccum >= stepAngle) {
        ++steps;
        touchTracker.angleAccum -= stepAngle;
    }
    while(touchTracker.angleAccum <= -stepAngle) {
        --steps;
        touchTracker.angleAccum += stepAngle;
    }

    if(steps != 0) {
        touchTracker.dragUsed = true;
        handleDialDelta(steps);
    }
}

void updateMenuScrollTouch(int16_t y) {
    const Layout layout = buildLayout();
    const int16_t threshold = static_cast<int16_t>(layout.rowH / 2);
    touchTracker.scrollAccum += static_cast<int16_t>(y - touchTracker.lastY);
    touchTracker.lastY = y;

    int steps = 0;
    while(touchTracker.scrollAccum >= threshold) {
        --steps;
        touchTracker.scrollAccum -= threshold;
    }
    while(touchTracker.scrollAccum <= -threshold) {
        ++steps;
        touchTracker.scrollAccum += threshold;
    }

    if(steps != 0) {
        touchTracker.dragUsed = true;
        moveSelection(steps);
    }
}

void handleTabsTouch(int16_t x, int16_t y) {
    const Layout layout = buildLayout();
    for(uint8_t i = 0; i < 5; ++i) {
        if(layout.tabs[i].contains(x, y)) {
            setScreen(kTabScreens[i]);
            return;
        }
    }
}

void handleHomeTouch(int16_t x, int16_t y) {
    const Layout layout = buildLayout();
    if(layout.homeModeButton.contains(x, y)) {
        toggleModeAction();
        return;
    }

    if(layout.homeActionButton.contains(x, y)) {
        sendAction(isRunningState() ? "stop" : "start");
        app.lastStatusPollAt = 0;
        setToast(isRunningState() ? "Stop sent" : "Start sent");
    }
}

void handleMenuTouch(int16_t x, int16_t y) {
    const Layout layout = buildLayout();

    if(app.editing) {
        if(layout.editCancel.contains(x, y)) {
            cancelEditing();
            return;
        }
        if(layout.editSave.contains(x, y)) {
            activateSelectedItem();
            return;
        }
        return;
    }

    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items == nullptr || count == 0) {
        return;
    }

    for(uint8_t row = 0; row < layout.rowCount; ++row) {
        const int idx = layout.firstRow + row;
        if(idx >= static_cast<int>(count)) {
            break;
        }
        if(layout.menuRows[row].contains(x, y)) {
            if(selectionFor(app.screen) == idx) {
                activateSelectedItem();
            } else {
                selectionFor(app.screen) = static_cast<uint8_t>(idx);
                app.redraw = true;
            }
            return;
        }
    }
}

void handleTouchTap(int16_t x, int16_t y) {
    handleTabsTouch(x, y);
    const Layout layout = buildLayout();
    if(y >= layout.tabs[0].y) {
        return;
    }

    if(app.screen == Screen::Home) {
        handleHomeTouch(x, y);
    } else {
        handleMenuTouch(x, y);
    }
}

void handleTouchInput(const RemoteTouchState& touch) {
    if(touch.justPressed) {
        beginTouchTracking(touch.x, touch.y);
        return;
    }

    if(touch.pressed && touchTracker.active) {
        const int32_t dx = static_cast<int32_t>(touch.x) - touchTracker.startX;
        const int32_t dy = static_cast<int32_t>(touch.y) - touchTracker.startY;
        if((dx * dx) + (dy * dy) >= static_cast<int32_t>(kTapMoveThreshold) * kTapMoveThreshold) {
            touchTracker.dragUsed = touchTracker.dragUsed || touchTracker.mode != TouchMode::Tap;
        }

        switch(touchTracker.mode) {
            case TouchMode::HomeDial:
                updateDialTouch(touch.x, touch.y, false);
                break;
            case TouchMode::EditorDial:
                updateDialTouch(touch.x, touch.y, true);
                break;
            case TouchMode::MenuScroll:
                updateMenuScrollTouch(touch.y);
                break;
            case TouchMode::Tap:
            default:
                break;
        }

        touchTracker.lastX = touch.x;
        touchTracker.lastY = touch.y;
        return;
    }

    if(touch.justReleased && touchTracker.active) {
        const uint32_t heldMs = millis() - touchTracker.startedAt;
        if(!touchTracker.dragUsed && heldMs <= remote_config::kTapMaxDurationMs) {
            handleTouchTap(touchTracker.startX, touchTracker.startY);
        }
        touchTracker = {};
    }
}

}  // namespace

void remote_app_setup() {
    Serial.begin(115200);
    delay(100);
    remote_board_console().println("SG98 multi-board remote starting");

    if(!remote_board_init()) {
        setToast("Board init failed", 60000);
    }

    remote_wifi_init();
    wsClient.onEvent(onWsEvent);
    seedMenuDefaults();
    app.manualRpm = default_setRPM;
    app.weightMg = default_setWeight;
    app.preferGbw = false;
    app.redraw = true;
    app.lastUserActivityAt = millis();
    app.currentTransport = LinkTransport::None;
    app.lastTransportSwitchAt = millis();
    refreshBatteryStatus(true);

    switchTransportNow(initialTransport());
    requestStatus();
    requestMenu(1);
    requestMenu(2);
    requestMenu(3);
}

void remote_app_loop() {
    RemoteInputState input = {};
    remote_board_poll_input(&input);

    if(input.dialDelta != 0 || input.primaryLongPress || input.primaryShortPress ||
       input.touch.pressed || input.touch.justPressed || input.touch.justReleased) {
        markUserActivity();
    }

    manageTransport();
    if(input.primaryLongPress) {
        handlePrimaryLongPress();
    }
    if(input.primaryShortPress) {
        handlePrimaryShortPress();
    }

    handleTouchInput(input.touch);
    handleDialDelta(input.dialDelta);
    commitTargetIfDue();
    syncPolling();
    refreshBatteryStatus(false);
    handleSendCompletion();
    updateSyncFromStatusFallback();
    remote_board_apply_indicator(currentIndicator());
    enterIdleSleepIfNeeded();

    if(app.redraw) {
        redrawUi();
    }

    delay(12);
}
