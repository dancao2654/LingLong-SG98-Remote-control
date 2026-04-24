#include "remote_app.h"

#include <math.h>
#include <string.h>

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "remote_board.h"
#include "remote_audio.h"
#include "remote_config.h"
#include "remote_runtime.h"
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
constexpr uint32_t kWheelAnimationFrameMs = 500;
constexpr float kWheelVisualTurnsPerSecond = 0.24f;
constexpr uint32_t kRoundInputQuietMs = 240;
constexpr uint32_t kTouchDuplicateTapSuppressMs = 140;
constexpr int16_t kTouchDuplicateTapRadius = 28;
constexpr int16_t kRoundHomeDialTouchInnerInset = 0;
constexpr int16_t kRoundHomeDialTouchOuterPad = 34;
constexpr int16_t kRoundHomeSwipeThreshold = 14;
constexpr int16_t kRoundHomeSwipeAxisSlack = 4;
constexpr int16_t kRoundHomeDialSwipeThreshold = 18;
constexpr int16_t kRoundHomeDialSwipeAxisSlack = 6;
constexpr int16_t kRoundMenuTabSwipeThreshold = 28;
constexpr int16_t kRoundMenuTabSwipeAxisSlack = 8;
constexpr bool kRoundInputDebug = false;
constexpr bool kDisableTransportPerfTest = false;
constexpr uint32_t kTransportUiYieldMs = 280;
constexpr uint32_t kHomeTransportQuietMs = 1000;
constexpr uint32_t kHomeTransportServiceMs = 450;
constexpr uint32_t kHomePollingServiceMs = 650;
constexpr uint32_t kHomeStatusPollIdleMs = 1800;
constexpr uint32_t kHomeStatusPollRunningMs = 1200;
constexpr uint32_t kHomeStatusPollActiveMs = 2400;
constexpr uint32_t kPersistentStateDebounceMs = 1200;
constexpr uint32_t kMotionHomeConfirmWindowMs = 1200;
constexpr uint32_t kMotionHomeMinGapMs = 250;
constexpr uint32_t kMotionHomeCooldownMs = 2500;
constexpr uint32_t kMotionHomeIgnoreAfterControlMs = 1200;
constexpr char kPersistentStateNamespace[] = "remote-app";
constexpr char kPersistentStateKey[] = "snapshot";

enum class Screen : uint8_t {
    Home = 0,
    Main = 1,
    Purge = 2,
    Gbw = 3,
    AudioDiag = 4,
    Drive = 5,
    EspNowPeers = 6,
    Network = 7,
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

constexpr uint8_t kScreenCount = 8;
constexpr uint8_t kEspNowPeerSlots = 6;
constexpr uint8_t kPendingMenuWriteSlots = 8;
constexpr uint32_t kEspNowDiscoveryIntervalMs = 1200;
constexpr uint32_t kEspNowDiscoveryExpireMs = 5000;
constexpr uint32_t kEspNowSendConfirmTimeoutMs = 450;
constexpr uint32_t kEspNowSendRetryBackoffMs = 80;
constexpr uint8_t kEspNowReliableSendAttempts = 6;
constexpr uint8_t kEspNowBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct DiscoveredEspNowPeer {
    bool valid = false;
    uint8_t mac[6] = {};
    uint8_t channel = 0;
    uint32_t seenAt = 0;
    String name;
    String target;
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
    Rect navPrev;
    Rect navHome;
    Rect navNext;
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
    HomeEdgeSwipe,
};

struct TouchTracker {
    bool active = false;
    bool dragUsed = false;
    bool circularPreferred = false;
    TouchMode mode = TouchMode::Tap;
    int16_t startX = 0;
    int16_t startY = 0;
    int16_t lastX = 0;
    int16_t lastY = 0;
    int16_t scrollAccum = 0;
    int16_t previewAudioSteps = 0;
    float lastAngle = 0.0f;
    float angleAccum = 0.0f;
    uint32_t startedAt = 0;
};

struct HomePalette {
    uint16_t background = kColorBg;
    uint16_t panel = kColorPanel;
    uint16_t panelHi = kColorPanelHi;
    uint16_t accent = kColorAccent;
    uint16_t accent2 = kColorAccent2;
    uint16_t muted = kColorMuted;
    uint16_t modeFill = kColorAccent;
};

enum class PendingActionKind : uint8_t {
    None = 0,
    Start,
    Stop,
    Idle,
    IdleGbw,
    Calibrate,
    DriveAuto,
    DriveJmc,
    DriveRt,
    JmcBaud,
    ResetDefaults,
};

enum class PendingActionReflection : uint8_t {
    None = 0,
    State,
    Drive,
    JmcBaud,
};

struct PendingTargetWrite {
    bool active = false;
    bool weightMode = false;
    int32_t value = 0;
    uint32_t queuedAt = 0;
    uint32_t lastSendAt = 0;
    uint16_t attempts = 0;
};

struct PendingMenuWrite {
    bool active = false;
    uint8_t menuNum = 0;
    uint8_t itemIdx = 0;
    int32_t value = 0;
    uint32_t queuedAt = 0;
    uint32_t lastSendAt = 0;
    uint16_t attempts = 0;
};

struct PendingAction {
    PendingActionKind kind = PendingActionKind::None;
    PendingActionReflection reflection = PendingActionReflection::None;
    int32_t value = 0;
    uint32_t queuedAt = 0;
    uint32_t lastSendAt = 0;
    uint16_t attempts = 0;
    char arg0[16] = {};
    char expectedText[24] = {};
};

struct AppState {
    Screen screen = Screen::Home;
    uint8_t selection[kScreenCount] = {};
    bool editing = false;
    bool targetDirty = false;
    int32_t draftValue = 0;
    int32_t manualRpm = default_setRPM;
    int32_t weightMg = default_setWeight;
    int32_t localInvertScroll = 0;
    int32_t localUiSounds = 1;
    int32_t localDriveSelection = 0;
    int32_t menuValues[5][sg98_protocol::kMaxMenuValues] = {};
    int32_t controllerMenuValues[5][sg98_protocol::kMaxMenuValues] = {};
    bool menuSynced[5] = {};
    bool controllerMenuSynced[5] = {};
    sg98_protocol::StatusFrame status;
    sg98_protocol::StatusFrame controllerStatus;
    bool hasStatus = false;
    bool hasControllerStatus = false;
    bool preferGbw = false;
    bool redraw = true;
    bool radioModeKnown = false;
    bool espNowPeerConfigured = false;
    bool espNowReady = false;
    bool wsConnected = false;
    bool wsClientStarted = false;
    bool mdnsReady = false;
    bool wsDiscoveryPending = false;
    bool wsStaConnected = false;
    bool sendComplete = false;
    bool sendOk = false;
    LinkTransport currentTransport = LinkTransport::None;
    LinkTransport pendingTransport = LinkTransport::None;
    uint32_t targetCommitAt = 0;
    uint32_t pendingTransportAt = 0;
    uint32_t lastTransportSwitchAt = 0;
    uint32_t toastUntil = 0;
    uint32_t lastLinkActivityAt = 0;
    uint32_t lastAckAt = 0;
    uint32_t lastStatusAt = 0;
    uint32_t lastStatusPollAt = 0;
    uint32_t lastBackgroundMenuPollAt = 0;
    uint32_t lastMdnsQueryAt = 0;
    uint32_t lastWifiServiceAt = 0;
    uint32_t lastWebSocketServiceAt = 0;
    uint32_t lastTransportServiceAt = 0;
    uint32_t lastPollingServiceAt = 0;
    uint32_t lastRoundInputDebugAt = 0;
    uint32_t lastMenuPollAt[5] = {};
    uint32_t lastMenuRxAt[5] = {};
    uint32_t lastEspNowDiscoveryAt = 0;
    bool transportYieldedToUi = false;
    bool transportOverrideActive = false;
    uint32_t dangerArmUntil = 0;
    Screen dangerScreen = Screen::Home;
    uint8_t dangerSelection = 0;
    uint32_t lastUserActivityAt = 0;
    uint32_t lastControlActivityAt = 0;
    uint32_t motionHomeCandidateAt = 0;
    uint32_t lastMotionHomeAt = 0;
    uint32_t lastTouchTapAt = 0;
    uint32_t lastBatteryPollAt = 0;
    uint32_t lastHomeAdjustAt = 0;
    uint32_t homeWheelAnimatedAt = 0;
    uint32_t homeWheelFrameAt = 0;
    int16_t lastTouchTapX = 0;
    int16_t lastTouchTapY = 0;
    RemoteBatteryStatus battery;
    PendingTargetWrite pendingTarget;
    PendingAction pendingAction;
    PendingMenuWrite pendingMenuWrites[kPendingMenuWriteSlots];
    uint8_t pendingMenuCursor = 0;
    uint8_t espNowPeerMac[6] = {};
    DiscoveredEspNowPeer espNowPeers[kEspNowPeerSlots];
    float homeWheelAngle = 0.0f;
    float menuRollOffset = 0.0f;
    String wsHost;
    String wsEndpointLabel;
    String mdnsHostName;
    uint16_t wsPort = remote_config::kWebSocketPort;
    String toast = "Booting";
};

struct PersistedStatusFrame {
    char state[24] = {};
    char drive[16] = {};
    char comm[20] = {};
    char motor[20] = {};
    char scale[32] = {};
    int32_t error = 0;
    int32_t setRpm = 0;
    int32_t motorSetRpm = 0;
    int32_t currentRpm = 0;
    int32_t torquePercent = 0;
    int32_t setWeightMg = 0;
    int32_t currentWeightMg = 0;
    int32_t currentMenu = 0;
    bool enterMenu = false;
    int32_t jmcBaud = 0;
    int32_t preferredJmcBaud = 0;
};

struct SleepSnapshot {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t screen = 0;
    uint8_t selection[kScreenCount] = {};
    int32_t manualRpm = default_setRPM;
    int32_t weightMg = default_setWeight;
    int32_t localInvertScroll = 0;
    int32_t localUiSounds = 1;
    int32_t localDriveSelection = 0;
    int32_t menuValues[5][sg98_protocol::kMaxMenuValues] = {};
    uint8_t menuSyncedMask = 0;
    uint8_t transport = 0;
    bool preferGbw = false;
    bool radioModeKnown = false;
    bool espNowPeerConfigured = false;
    uint8_t espNowPeerMac[6] = {};
    bool hasStatus = false;
    PersistedStatusFrame status = {};
};

template <size_t N>
constexpr size_t arrayCount(const SettingItem (&)[N]) {
    return N;
}

bool isInvertScrollItem(const SettingItem& item);
bool isConfirmActionItem(const SettingItem& item);
bool usesChoiceEditor(const SettingItem& item);
bool hasActiveEspNowPeer();
bool isEspNowPeerSlotItem(const SettingItem& item);
bool isEspNowPeerScanItem(const SettingItem& item);
bool isEspNowPeerClearItem(const SettingItem& item);
bool isControllerLinkItem(const SettingItem& item);
String espNowPeerLabelForItem(const SettingItem& item);
bool pendingControllerSyncActive();
void requestWebSocketDiscovery(bool resetSession);
void remoteConsolePoll();
void remoteConsoleMaybeStream();
void remoteConsoleTraceFrame(const char* direction, const String& frame);

static_assert(NUM_MENU1_ITEMS <= sg98_protocol::kMaxMenuValues, "menu1 frame too small");
static_assert(NUM_MENU2_ITEMS <= sg98_protocol::kMaxMenuValues, "menu2 frame too small");
static_assert(NUM_MENU3_ITEMS <= sg98_protocol::kMaxMenuValues, "menu3 frame too small");
static_assert(NUM_MENU4_ITEMS <= sg98_protocol::kMaxMenuValues, "menu4 frame too small");

constexpr SettingItem kMainItems[] = {
    {"Sleep time", ItemKind::Value, 1, SETSLEEP, 0, 300, 5, "min", nullptr, nullptr},
    {"Brightness", ItemKind::Value, 1, DISP_BRIGHTNESS, 1, 255, 1, "", nullptr, nullptr},
    {"LED max Brightness", ItemKind::Value, 1, LED_BRIGHTNESS, 0, 100, 1, "%", nullptr, nullptr},
    {"Reset default", ItemKind::Danger, 0, 0, 0, 0, 0, "", "reset_defaults", "confirm"},
};

constexpr SettingItem kPurgeItems[] = {
    {"Auto purge", ItemKind::Toggle, 2, AUTO_PURGE_ENABLED, 0, 1, 1, "", nullptr, nullptr},
    {"Purge /w button", ItemKind::Toggle, 2, SETBUTTONPURGE, 0, 1, 1, "", nullptr, nullptr},
    {"Purge forward RPM", ItemKind::Value, 2, SETPURGERPM, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Reverse rotation", ItemKind::Value, 2, SETPURGEREVERSE, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Purge delay", ItemKind::Value, 2, SETPURGEDELAY, 100, 10000, 50, "ms", nullptr, nullptr},
    {"Purge duration", ItemKind::Value, 2, SETPURGETIME, 500, 5000, 100, "ms", nullptr, nullptr},
    {"Purge 0rpm time", ItemKind::Value, 2, SETPURGEOFFTIME, 0, 5000, 10, "ms", nullptr, nullptr},
    {"Purge stabilize time", ItemKind::Value, 2, SETPURGESTABILTIME, 100, 5000, 50, "ms", nullptr, nullptr},
    {"Purge frames HIGH", ItemKind::Value, 2, SETPURGEFRAMESHIGH, 2, 20, 1, "", nullptr, nullptr},
    {"Purge frames LOW", ItemKind::Value, 2, SETPURGEFRAMESLOW, 2, 20, 1, "", nullptr, nullptr},
    {"Purge Scalar HIGH", ItemKind::Value, 2, SETPURGEPRCTHIGH, 101, 200, 1, "%", nullptr, nullptr},
    {"Purge Scalar LOW", ItemKind::Value, 2, SETPURGEPRCTLOW, 101, 200, 1, "%", nullptr, nullptr},
    {"Auto off time", ItemKind::Value, 2, AUTO_OFF_TIME, 0, 300, 10, "s", nullptr, nullptr},
};

constexpr SettingItem kGbwItems[] = {
    {"Max RPM", ItemKind::Value, 1, SETMAX, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Min RPM", ItemKind::Value, 1, SETMIN, absolute_min_rpm, 500, rpm_scalar, "rpm", nullptr, nullptr},
    {"Motor torque", ItemKind::Value, 1, SETMOTORTORQUE, 1, 300, 1, "%", nullptr, nullptr},
    {"Motor ramp", ItemKind::Value, 1, SETMOTORRAMP, 0, 12, 1, "lvl", nullptr, nullptr},
    {"Motor direction", ItemKind::Toggle, 1, SETMOTORDIRECTION, 0, 1, 1, "", nullptr, nullptr},
    {"Drive", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "drive"},
    {"Calibrate", ItemKind::Action, 0, 0, 0, 0, 0, "", "calibrate", nullptr},
    {"GbW RPM", ItemKind::Value, 3, GBW_RPM_SET, 200, 1500, rpm_scalar, "rpm", nullptr, nullptr},
    {"GbW Slow RPM", ItemKind::Value, 3, GBW_SLOW_RPM, rpm_scalar, 500, rpm_scalar, "rpm", nullptr, nullptr},
    {"GbW Slow mg", ItemKind::Value, 3, GBW_SLOW_MG, 0, 6000, 100, "mg", nullptr, nullptr},
    {"Start delay", ItemKind::Value, 3, GBW_BUTTON_DELAY, 0, 1000, 50, "ms", nullptr, nullptr},
    {"SpeedModifier", ItemKind::Value, 3, GBW_SPEEDMOD, 1, 32767, 100, "", nullptr, nullptr},
    {"Time offset", ItemKind::Value, 3, GBW_OFFSET, 10, 1000, 10, "ms", nullptr, nullptr},
    {"Save scale", ItemKind::Toggle, 3, SAVE_SCALE, 0, 1, 1, "", nullptr, nullptr},
};

constexpr SettingItem kNetworkItems[] = {
    {"Radio mode", ItemKind::Toggle, 1, RADIOMODE, 0, 1, 1, "", nullptr, nullptr},
    {"Controller link", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "peers"},
    {"Audio diag", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "audio"},
    {"Reset Network", ItemKind::Action, 0, 0, 0, 0, 0, "", "reset_network", nullptr},
};

constexpr SettingItem kAudioItems[] = {
    {"UI sounds", ItemKind::Toggle, 0, 0, 0, 1, 1, "", "ui_sounds", nullptr},
    {"Probe codec", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_probe", nullptr},
    {"Play click", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_click", nullptr},
    {"Play swipe", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_swipe", nullptr},
    {"Play sweep", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_sweep", nullptr},
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

constexpr SettingItem kEspNowPeerItems[] = {
    {"Controller 1", ItemKind::Action, 0, 0, 0, 0, 0, "", "peer_slot", nullptr},
    {"Controller 2", ItemKind::Action, 0, 1, 0, 0, 0, "", "peer_slot", nullptr},
    {"Controller 3", ItemKind::Action, 0, 2, 0, 0, 0, "", "peer_slot", nullptr},
    {"Controller 4", ItemKind::Action, 0, 3, 0, 0, 0, "", "peer_slot", nullptr},
    {"Controller 5", ItemKind::Action, 0, 4, 0, 0, 0, "", "peer_slot", nullptr},
    {"Controller 6", ItemKind::Action, 0, 5, 0, 0, 0, "", "peer_slot", nullptr},
    {"Scan again", ItemKind::Action, 0, 6, 0, 0, 0, "", "peer_scan", nullptr},
    {"Forget lock", ItemKind::Action, 0, 7, 0, 0, 0, "", "peer_clear", nullptr},
};

constexpr Screen kTabScreens[] = {
    Screen::Home,
    Screen::Main,
    Screen::Purge,
    Screen::Gbw,
    Screen::Network,
};

constexpr const char* kTabLabels[] = {"RUN", "MAIN", "PURGE", "GRIND", "NET"};

constexpr Screen kRoundMenuTabScreens[] = {
    Screen::Main,
    Screen::Purge,
    Screen::Gbw,
    Screen::Network,
};

AppState app;
TouchTracker touchTracker;
WebSocketsClient wsClient;
RTC_DATA_ATTR SleepSnapshot g_sleepSnapshot;
Preferences g_statePrefs;
SleepSnapshot g_savedPersistentSnapshot;
bool g_statePrefsReady = false;
bool g_savedPersistentSnapshotValid = false;
bool g_persistentStateDirty = false;
uint32_t g_persistentStateDueAt = 0;
uint8_t g_appliedBacklightLevel = 0;
bool g_backlightLevelValid = false;
bool g_consoleStreamEnabled = false;
bool g_consoleFrameTraceEnabled = false;
uint32_t g_consoleLastStreamAt = 0;
char g_consoleLineBuffer[160] = {};
size_t g_consoleLineLen = 0;

constexpr uint32_t kSleepSnapshotMagic = 0x53473938UL;
constexpr uint16_t kSleepSnapshotVersion = 7;
constexpr uint16_t kSleepSnapshotVersionLegacyUiSoundsOff = 6;
constexpr uint32_t kInteractiveLoopWaitMs = 4;
constexpr uint32_t kAnimatingLoopWaitMs = 16;
constexpr uint32_t kIdleLoopWaitCapMs = 80;

enum LocalDriveSelection : int32_t {
    LOCAL_DRIVE_AUTO = 0,
    LOCAL_DRIVE_JMC = 1,
    LOCAL_DRIVE_RT = 2,
};

bool useRoundNavigation();
bool roundUiInteractionActive();
bool isRunningState();

uint32_t currentWebSocketLoopIntervalMs() {
    return app.wsConnected ? remote_config::kWebSocketServiceMs
                           : remote_config::kWebSocketServiceIdleMs;
}

uint32_t msUntilDeadline(uint32_t now, uint32_t deadline) {
    const int32_t remaining = static_cast<int32_t>(deadline - now);
    return remaining <= 0 ? 0u : static_cast<uint32_t>(remaining);
}

void shortenWaitForDeadline(uint32_t now, uint32_t deadline, uint32_t* waitMs) {
    if(waitMs == nullptr || deadline == 0) {
        return;
    }

    const uint32_t remaining = msUntilDeadline(now, deadline);
    if(remaining < *waitMs) {
        *waitMs = remaining;
    }
}

uint32_t currentTransportServiceIntervalMs() {
    if(pendingControllerSyncActive()) {
        if(app.currentTransport == LinkTransport::WebSocket ||
           app.pendingTransport == LinkTransport::WebSocket ||
           remote_wifi_is_active()) {
            return app.wsConnected ? remote_config::kWebSocketServiceMs
                                   : currentWebSocketLoopIntervalMs();
        }
        return remote_config::kTransportServiceMs;
    }
    if(app.currentTransport == LinkTransport::WebSocket ||
       app.pendingTransport == LinkTransport::WebSocket ||
       remote_wifi_is_active()) {
        return app.wsClientStarted ? currentWebSocketLoopIntervalMs()
                                   : remote_config::kTransportServiceMs;
    }
    if(useRoundNavigation() && app.screen == Screen::Home) {
        return kHomeTransportServiceMs;
    }
    return remote_config::kTransportServiceMs;
}

uint32_t currentPollingServiceIntervalMs() {
    if(pendingControllerSyncActive()) {
        return remote_config::kPollingServiceMs;
    }
    if(useRoundNavigation() && app.screen == Screen::Home) {
        return kHomePollingServiceMs;
    }
    return remote_config::kPollingServiceMs;
}

uint32_t computeLoopWaitMs(uint32_t now, bool yieldTransport) {
    if(kDisableTransportPerfTest) {
        return kInteractiveLoopWaitMs;
    }

    if(app.redraw || Serial.available() > 0 || remote_board_input_pending()) {
        return 0;
    }

    uint32_t waitMs = kIdleLoopWaitCapMs;

    if(touchTracker.active || app.targetDirty || app.editing) {
        waitMs = kInteractiveLoopWaitMs;
    } else if(roundUiInteractionActive() || fabsf(app.menuRollOffset) >= 0.5f) {
        waitMs = kAnimatingLoopWaitMs;
    }

    if(useRoundNavigation() && app.screen == Screen::Home) {
        shortenWaitForDeadline(now,
                               app.lastUserActivityAt + kRoundInputQuietMs,
                               &waitMs);
        if(yieldTransport) {
            shortenWaitForDeadline(now,
                                   app.lastUserActivityAt + kHomeTransportQuietMs,
                                   &waitMs);
        }
    }

    if(app.pendingTransport != LinkTransport::None) {
        shortenWaitForDeadline(now, app.pendingTransportAt, &waitMs);
    }

    if(g_persistentStateDirty) {
        shortenWaitForDeadline(now, g_persistentStateDueAt, &waitMs);
    }

    if(app.targetDirty) {
        shortenWaitForDeadline(now, app.targetCommitAt, &waitMs);
    }

    if(app.toastUntil > now) {
        shortenWaitForDeadline(now, app.toastUntil, &waitMs);
    }

    if(g_consoleStreamEnabled) {
        shortenWaitForDeadline(now, g_consoleLastStreamAt + 1000, &waitMs);
    }

    if(!kDisableTransportPerfTest && !yieldTransport) {
        shortenWaitForDeadline(now,
                               app.lastTransportServiceAt + currentTransportServiceIntervalMs(),
                               &waitMs);
        shortenWaitForDeadline(now,
                               app.lastPollingServiceAt + currentPollingServiceIntervalMs(),
                               &waitMs);
    }

    if(useRoundNavigation() && app.screen == Screen::Home && isRunningState() &&
       !roundUiInteractionActive() && app.homeWheelFrameAt != 0) {
        shortenWaitForDeadline(now, app.homeWheelFrameAt + kWheelAnimationFrameMs, &waitMs);
    }

    if(!remote_config::kDisableIdleSleepForTesting) {
        shortenWaitForDeadline(now, app.lastUserActivityAt + remote_config::kIdleSleepAfterMs,
                               &waitMs);
    }
    return waitMs;
}

template <size_t N>
void copyStringToBuffer(const String& value, char (&dest)[N]) {
    value.toCharArray(dest, N);
}

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

LinkTransport sanitizeTransport(uint8_t raw) {
    switch(static_cast<LinkTransport>(raw)) {
        case LinkTransport::EspNow:
        case LinkTransport::WebSocket:
            return static_cast<LinkTransport>(raw);
        case LinkTransport::None:
        default:
            return LinkTransport::None;
    }
}

void setToast(const String& text, uint32_t durationMs);
void setScreen(Screen screen);
void sanitizeSelections();
void markPersistentStateDirty(uint32_t debounceMs);

void clearRecoveredSendFailureToast() {
    if(app.toast == "Send failed" && app.toastUntil > millis()) {
        app.toast = "";
        app.toastUntil = 0;
        app.redraw = true;
    }
}

bool prefersWebSocket() {
    return (app.radioModeKnown && transportFromRadioMode(app.menuValues[1][RADIOMODE]) == LinkTransport::WebSocket) ||
           app.pendingTransport == LinkTransport::WebSocket;
}

bool controllerLinkUsesWebSocketScan() {
    if(app.radioModeKnown) {
        return transportFromRadioMode(app.menuValues[1][RADIOMODE]) == LinkTransport::WebSocket;
    }
    return app.currentTransport == LinkTransport::WebSocket ||
           app.pendingTransport == LinkTransport::WebSocket;
}

bool webSocketCandidateAvailable() {
    return remote_wifi_has_saved_network() || prefersWebSocket();
}

LinkTransport initialTransport() {
    if(transportFromRadioMode(app.menuValues[1][RADIOMODE]) == LinkTransport::EspNow) {
        return LinkTransport::EspNow;
    }
    if(remote_wifi_has_saved_network()) {
        return LinkTransport::WebSocket;
    }
    if(hasActiveEspNowPeer()) {
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

float clampUnit(float value) {
    if(value < 0.0f) return 0.0f;
    if(value > 1.0f) return 1.0f;
    return value;
}

float smoothUnit(float value) {
    const float clamped = clampUnit(value);
    return clamped * clamped * (3.0f - (2.0f * clamped));
}

uint16_t packColor565(uint8_t red, uint8_t green, uint8_t blue) {
    return static_cast<uint16_t>(((red & 0xF8) << 8) |
                                 ((green & 0xFC) << 3) |
                                 (blue >> 3));
}

uint8_t expand5To8(uint8_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t expand6To8(uint8_t value) {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

uint16_t blendColor565(uint16_t from, uint16_t to, float amount) {
    const float t = clampUnit(amount);
    const uint8_t fromR = expand5To8(static_cast<uint8_t>((from >> 11) & 0x1F));
    const uint8_t fromG = expand6To8(static_cast<uint8_t>((from >> 5) & 0x3F));
    const uint8_t fromB = expand5To8(static_cast<uint8_t>(from & 0x1F));
    const uint8_t toR = expand5To8(static_cast<uint8_t>((to >> 11) & 0x1F));
    const uint8_t toG = expand6To8(static_cast<uint8_t>((to >> 5) & 0x3F));
    const uint8_t toB = expand5To8(static_cast<uint8_t>(to & 0x1F));
    const uint8_t mixedR = static_cast<uint8_t>(roundf(fromR + (toR - fromR) * t));
    const uint8_t mixedG = static_cast<uint8_t>(roundf(fromG + (toG - fromG) * t));
    const uint8_t mixedB = static_cast<uint8_t>(roundf(fromB + (toB - fromB) * t));
    return packColor565(mixedR, mixedG, mixedB);
}

bool isLargeCanvas() {
    return remote_board_info().width >= 380;
}

bool useRoundNavigation() {
    return isLargeCanvas();
}

bool useWaveshareRoundUi() {
    switch(remote_board_info().id) {
        case RemoteBoardId::WaveshareAmoled175:
        case RemoteBoardId::CrowPanelRotary21:
            return true;
        default:
            return false;
    }
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

RemoteTextStyle pillTextStyle() {
    return isLargeCanvas() ? RemoteTextStyle::Small : bodyTextStyle();
}

RemoteTextStyle menuLabelTextStyle() {
    return isLargeCanvas() ? RemoteTextStyle::Small : bodyTextStyle();
}

RemoteTextStyle menuValueTextStyle() {
    return isLargeCanvas() ? RemoteTextStyle::Tiny : bodyTextStyle();
}

uint8_t textScale(RemoteTextStyle style) {
    switch(style) {
        case RemoteTextStyle::Tiny: return 1;
        case RemoteTextStyle::Small: return 2;
        case RemoteTextStyle::Medium: return 3;
        case RemoteTextStyle::Large: return 4;
        case RemoteTextStyle::Hero: return 5;
        default: return 1;
    }
}

bool textFitsWidth(const String& text, int16_t width, RemoteTextStyle style, int16_t horizontalPadding = 12) {
    if(width <= horizontalPadding * 2) {
        return false;
    }

    const int16_t usable = width - horizontalPadding * 2;
    const int16_t charWidth = static_cast<int16_t>(6 * textScale(style));
    if(charWidth <= 0) {
        return true;
    }

    return static_cast<int16_t>(text.length()) * charWidth <= usable;
}

int16_t estimatedTextWidth(const String& text, RemoteTextStyle style) {
    return static_cast<int16_t>(text.length()) * static_cast<int16_t>(6 * textScale(style));
}

RemoteTextStyle chooseValueTextStyle(const String& text, int16_t width) {
    const RemoteTextStyle candidates[] = {
        RemoteTextStyle::Hero,
        RemoteTextStyle::Large,
        RemoteTextStyle::Medium,
        RemoteTextStyle::Small,
    };
    for(RemoteTextStyle style : candidates) {
        if(textFitsWidth(text, width, style, 12)) {
            return style;
        }
    }
    return RemoteTextStyle::Small;
}

RemoteTextStyle chooseLabelTextStyle(const String& text, int16_t width) {
    const RemoteTextStyle candidates[] = {
        RemoteTextStyle::Medium,
        RemoteTextStyle::Small,
        RemoteTextStyle::Tiny,
    };
    for(RemoteTextStyle style : candidates) {
        if(textFitsWidth(text, width, style, 16)) {
            return style;
        }
    }
    return RemoteTextStyle::Tiny;
}

RemoteTextStyle chooseMenuHeroTextStyle(const String& text, int16_t width) {
    const RemoteTextStyle candidates[] = {
        RemoteTextStyle::Large,
        RemoteTextStyle::Medium,
        RemoteTextStyle::Small,
        RemoteTextStyle::Tiny,
    };
    for(RemoteTextStyle style : candidates) {
        if(textFitsWidth(text, width, style, 18)) {
            return style;
        }
    }
    return RemoteTextStyle::Tiny;
}

String fitTextToWidth(const String& text, int16_t width, RemoteTextStyle style, int16_t horizontalPadding = 12) {
    if(width <= horizontalPadding * 2) {
        return "";
    }

    const int16_t usable = width - horizontalPadding * 2;
    const int16_t charWidth = static_cast<int16_t>(6 * textScale(style));
    if(charWidth <= 0) {
        return text;
    }

    int maxChars = usable / charWidth;
    if(maxChars <= 0) {
        return "";
    }
    if(static_cast<int>(text.length()) <= maxChars) {
        return text;
    }
    if(maxChars <= 3) {
        return text.substring(0, maxChars);
    }
    return text.substring(0, maxChars - 1) + String("~");
}

String fitTextToWidthNoTilde(const String& text, int16_t width, RemoteTextStyle style, int16_t horizontalPadding = 12) {
    if(width <= horizontalPadding * 2) {
        return "";
    }

    const int16_t usable = width - horizontalPadding * 2;
    const int16_t charWidth = static_cast<int16_t>(6 * textScale(style));
    if(charWidth <= 0) {
        return text;
    }

    int maxChars = usable / charWidth;
    if(maxChars <= 0) {
        return "";
    }
    if(static_cast<int>(text.length()) <= maxChars) {
        return text;
    }
    return text.substring(0, maxChars);
}

RemoteTextStyle chooseArcTextStyle(const String& text, int16_t radius, float spanDegrees, bool prominent) {
    if(radius <= 0 || spanDegrees <= 0.0f) {
        return RemoteTextStyle::Tiny;
    }

    const float spanRadians = spanDegrees * 0.01745329252f;
    const int16_t arcLength = static_cast<int16_t>(roundf(static_cast<float>(radius) * spanRadians));
    const RemoteTextStyle candidates[] = {
        prominent ? RemoteTextStyle::Medium : RemoteTextStyle::Small,
        RemoteTextStyle::Small,
        RemoteTextStyle::Tiny,
    };
    for(RemoteTextStyle style : candidates) {
        if(textFitsWidth(text, arcLength, style, 8)) {
            return style;
        }
    }
    return RemoteTextStyle::Tiny;
}

String fitTextToArc(const String& text, int16_t radius, float spanDegrees, RemoteTextStyle style) {
    if(radius <= 0 || spanDegrees <= 0.0f) {
        return "";
    }

    const float spanRadians = spanDegrees * 0.01745329252f;
    const int16_t arcLength = static_cast<int16_t>(roundf(static_cast<float>(radius) * spanRadians));
    return fitTextToWidthNoTilde(text, arcLength, style, 8);
}

String upperText(const String& text) {
    String out = text;
    out.toUpperCase();
    return out;
}

bool isLinkOnline();

String roundHomeTopLabel() {
    if(app.hasStatus) {
        if(app.status.motor.length() > 0) {
            return upperText(app.status.motor);
        }
        if(app.status.drive.length() > 0) {
            return upperText(app.status.drive);
        }
    }
    return "SG98";
}

String roundHomeBottomLabel() {
    if(app.toastUntil > millis()) {
        return upperText(app.toast);
    }
    if(!isLinkOnline()) {
        if(app.currentTransport == LinkTransport::EspNow) {
            return hasActiveEspNowPeer() ? "CONTROLLER OFFLINE" : "ESP-NOW SCAN";
        }
        if(remote_wifi_is_active()) {
            if(remote_wifi_sta_connected()) {
                return "CONTROLLER OFFLINE";
            }
            return "CONNECTING WI-FI";
        }
        return "CONTROLLER OFFLINE";
    }
    if(remote_wifi_is_active()) {
        if(remote_wifi_sta_connected()) {
            return String("WI-FI ") + remote_wifi_sta_ip();
        }
        return String("WI-FI AP ") + remote_wifi_ap_ip();
    }
    if(app.currentTransport == LinkTransport::EspNow) {
        if(!hasActiveEspNowPeer()) {
            return "ESP-NOW SCAN";
        }
        return (app.hasStatus && (millis() - app.lastStatusAt) < remote_config::kLinkTimeoutMs) ? "ESP-NOW LINK" : "ESP-NOW";
    }
    return "";
}

String displayLabelForItem(const SettingItem& item) {
    if(isEspNowPeerSlotItem(item) || isEspNowPeerScanItem(item) || isEspNowPeerClearItem(item)) {
        return espNowPeerLabelForItem(item);
    }
    if(isControllerLinkItem(item)) {
        return controllerLinkUsesWebSocketScan() ? "SCAN CONTROLLER" : "CONTROLLER LINK";
    }
    if(strcmp(item.label, "Motor setting") == 0) return "MOTOR SET";
    return upperText(item.label);
}

String displayHintForItem(const SettingItem& item) {
    if(isEspNowPeerSlotItem(item)) {
        return "LOCK CONTROLLER";
    }
    if(isEspNowPeerScanItem(item)) {
        return "DISCOVER CONTROLLERS";
    }
    if(isEspNowPeerClearItem(item)) {
        return "REMOVE LOCK";
    }
    if(isControllerLinkItem(item)) {
        return controllerLinkUsesWebSocketScan() ? "DISCOVER WS HOST" : "PICK CONTROLLER";
    }
    if(item.action != nullptr && strcmp(item.action, "screen") == 0) {
        return "OPEN MENU";
    }

    if(isInvertScrollItem(item)) {
        return "CLOCKWISE OR COUNTER-CLOCKWISE";
    }

    if(strcmp(item.label, "Motor direction") == 0) {
        return "NORMAL OR REVERSED";
    }
    if(isConfirmActionItem(item)) {
        return "CHOOSE NO OR YES";
    }

    switch(item.kind) {
        case ItemKind::Toggle:
            return "ON OR OFF";
        case ItemKind::Action:
            return "TRIGGER";
        case ItemKind::Danger:
            return "ARMED ACTION";
        case ItemKind::Value:
        default:
            break;
    }

    if(item.unit != nullptr && item.unit[0] != '\0') {
        return upperText(item.unit);
    }

    return "LEVEL";
}

bool settingSupportsLevelRing(const SettingItem& item) {
    return item.kind == ItemKind::Value || usesChoiceEditor(item);
}

constexpr Screen kMenuScreens[] = {
    Screen::Main,
    Screen::Purge,
    Screen::Gbw,
    Screen::Network,
    Screen::AudioDiag,
};

const char* screenTitle(Screen screen) {
    switch(screen) {
        case Screen::Home: return "RUN";
        case Screen::Main: return "SYSTEM";
        case Screen::Purge: return "PURGE";
        case Screen::Gbw: return "GRINDER";
        case Screen::AudioDiag: return "AUDIO";
        case Screen::Drive: return "DRIVE";
        case Screen::EspNowPeers: return "ESP-NOW";
        case Screen::Network: return "NETWORK";
        default: return "?";
    }
}

Screen cycleMenuScreen(Screen current, int delta) {
    int index = 0;
    for(size_t i = 0; i < (sizeof(kMenuScreens) / sizeof(kMenuScreens[0])); ++i) {
        if(kMenuScreens[i] == current) {
            index = static_cast<int>(i);
            break;
        }
    }

    const int count = static_cast<int>(sizeof(kMenuScreens) / sizeof(kMenuScreens[0]));
    int next = index + delta;
    while(next < 0) next += count;
    while(next >= count) next -= count;
    return kMenuScreens[next];
}

Screen roundNavMenuTarget(Screen current, int delta) {
    if(current == Screen::Drive) {
        return Screen::Main;
    }
    if(current == Screen::EspNowPeers) {
        return Screen::Network;
    }
    if(current == Screen::AudioDiag) {
        return Screen::Network;
    }
    return cycleMenuScreen(current, delta);
}

Screen cycleTabScreen(Screen current, int delta) {
    int index = 0;
    for(size_t i = 0; i < (sizeof(kTabScreens) / sizeof(kTabScreens[0])); ++i) {
        if(kTabScreens[i] == current) {
            index = static_cast<int>(i);
            break;
        }
    }

    const int count = static_cast<int>(sizeof(kTabScreens) / sizeof(kTabScreens[0]));
    int next = index + delta;
    while(next < 0) next += count;
    while(next >= count) next -= count;
    return kTabScreens[next];
}

Screen cycleRoundMenuTabScreen(Screen current, int delta) {
    int index = -1;
    for(size_t i = 0; i < (sizeof(kRoundMenuTabScreens) / sizeof(kRoundMenuTabScreens[0])); ++i) {
        if(kRoundMenuTabScreens[i] == current) {
            index = static_cast<int>(i);
            break;
        }
    }

    if(index < 0) {
        return current;
    }

    const int count = static_cast<int>(sizeof(kRoundMenuTabScreens) / sizeof(kRoundMenuTabScreens[0]));
    int next = index + delta;
    while(next < 0) next += count;
    while(next >= count) next -= count;
    return kRoundMenuTabScreens[next];
}

bool isRoundTopLevelTabScreen(Screen screen) {
    for(size_t i = 0; i < (sizeof(kRoundMenuTabScreens) / sizeof(kRoundMenuTabScreens[0])); ++i) {
        if(kRoundMenuTabScreens[i] == screen) {
            return true;
        }
    }
    return false;
}

Screen stepTabScreen(Screen current, int delta) {
    int index = -1;
    for(size_t i = 0; i < (sizeof(kTabScreens) / sizeof(kTabScreens[0])); ++i) {
        if(kTabScreens[i] == current) {
            index = static_cast<int>(i);
            break;
        }
    }
    if(index < 0) {
        return current;
    }

    const int count = static_cast<int>(sizeof(kTabScreens) / sizeof(kTabScreens[0]));
    int next = index + delta;
    if(next < 0) {
        next = 0;
    }
    if(next >= count) {
        next = count - 1;
    }
    return kTabScreens[next];
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

String weightValueText(int32_t mg) {
    String out;
    const int32_t whole = mg / 1000;
    const int32_t frac = abs(mg % 1000);
    out.reserve(12);
    out += String(whole);
    out += '.';
    if(frac < 100) out += '0';
    if(frac < 10) out += '0';
    out += String(frac);
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
    return app.lastLinkActivityAt != 0 &&
           (millis() - app.lastLinkActivityAt) < remote_config::kLinkTimeoutMs;
}

bool isGbwState() {
    if(app.hasStatus) {
        if(app.status.state == "IDLE" || app.status.state == "IDLE_GBW") {
            return app.preferGbw;
        }
        return app.status.state == "IDLE_GBW" || app.status.state == "GRINDING_GBW";
    }
    return app.preferGbw;
}

bool isRunningState() {
    return app.hasStatus &&
           (app.status.state == "GRINDING" || app.status.state == "GRINDING_GBW" || app.status.state == "PURGING");
}

bool isRunActionAvailable() {
    return isLinkOnline();
}

String homeActionButtonLabel() {
    return isRunActionAvailable() ? (isRunningState() ? "STOP" : "START") : "NO LINK";
}

uint16_t homeActionButtonFill(const HomePalette& palette) {
    if(!isRunActionAvailable()) {
        return blendColor565(palette.panel, kColorDanger, 0.26f);
    }
    return isRunningState() ? kColorDanger : kColorGood;
}

uint16_t homeActionButtonTextColor() {
    return isRunActionAvailable() ? kColorBlack : kColorText;
}

String homePrimaryStatusLine() {
    if(!isLinkOnline()) {
        return "CONTROLLER OFFLINE";
    }
    return app.hasStatus ? app.status.state : "WAITING";
}

float manualRpmWarningLevel() {
    if(isGbwState()) {
        return 0.0f;
    }

    const int32_t minRpm = app.menuValues[1][SETMIN] > 0 ? app.menuValues[1][SETMIN] : default_minRPM;
    int32_t maxRpm = app.menuValues[1][SETMAX] > minRpm ? app.menuValues[1][SETMAX] : default_maxRPM;
    if(maxRpm <= minRpm) {
        maxRpm = minRpm + rpm_scalar;
    }

    const int32_t targetRpm = clampValue(app.manualRpm, minRpm, maxRpm);
    const float normalized = static_cast<float>(targetRpm - minRpm) / static_cast<float>(maxRpm - minRpm);
    return smoothUnit(normalized);
}

uint16_t rpmWarningHeatColor(float warningLevel) {
    const uint16_t lowColor = packColor565(66, 216, 128);
    const uint16_t midColor = packColor565(255, 194, 74);
    const uint16_t highColor = packColor565(255, 84, 52);
    const float clamped = clampUnit(warningLevel);
    if(clamped <= 0.58f) {
        return blendColor565(lowColor, midColor, clamped / 0.58f);
    }
    return blendColor565(midColor, highColor, (clamped - 0.58f) / 0.42f);
}

HomePalette buildHomePalette() {
    HomePalette palette = {};
    if(isGbwState()) {
        palette.modeFill = kColorWarn;
        return palette;
    }

    const float warningLevel = manualRpmWarningLevel();
    const uint16_t heat = rpmWarningHeatColor(warningLevel);
    palette.background = blendColor565(kColorBg, heat, 0.18f + warningLevel * 0.18f);
    palette.panel = blendColor565(kColorPanel, heat, 0.28f + warningLevel * 0.30f);
    palette.panelHi = blendColor565(kColorPanelHi, heat, 0.22f + warningLevel * 0.28f);
    palette.accent = blendColor565(kColorGood, heat, 0.82f);
    palette.accent2 = blendColor565(kColorAccent2, heat, 0.72f);
    palette.muted = blendColor565(kColorMuted, heat, 0.18f + warningLevel * 0.20f);
    palette.modeFill = blendColor565(kColorGood, heat, 0.68f);
    return palette;
}

int16_t homeWheelDisplayRpm() {
    if(!app.hasStatus) {
        return 0;
    }
    if(app.status.currentRpm != 0) {
        return app.status.currentRpm;
    }
    if(isRunningState() && app.status.motorSetRpm != 0) {
        return app.status.motorSetRpm;
    }
    if(isRunningState()) {
        return app.status.setRpm;
    }
    return 0;
}

int32_t rpmMagnitude(int32_t rpm) {
    return rpm < 0 ? -rpm : rpm;
}

int8_t homeWheelSpinDirection() {
    const int16_t rpm = homeWheelDisplayRpm();
    if(rpm > 0) {
        return 1;
    }
    if(rpm < 0) {
        return -1;
    }
    return 0;
}

float homeWheelTurnsPerSecond() {
    if(!isRunningState() || homeWheelSpinDirection() == 0) {
        return 0.0f;
    }
    return kWheelVisualTurnsPerSecond;
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

bool isDisplayBrightnessItem(const SettingItem& item) {
    return item.menuNum == 1 && item.itemIdx == DISP_BRIGHTNESS;
}

bool isInvertScrollItem(const SettingItem& item) {
    return item.menuNum == 1 && item.itemIdx == INVERT_SCROLL;
}

bool isUiSoundsItem(const SettingItem& item) {
    return item.action != nullptr && strcmp(item.action, "ui_sounds") == 0;
}

LocalDriveSelection sanitizeLocalDriveSelection(int32_t value) {
    switch(value) {
        case LOCAL_DRIVE_JMC: return LOCAL_DRIVE_JMC;
        case LOCAL_DRIVE_RT: return LOCAL_DRIVE_RT;
        case LOCAL_DRIVE_AUTO:
        default:
            return LOCAL_DRIVE_AUTO;
    }
}

LocalDriveSelection currentLocalDriveSelection() {
    return sanitizeLocalDriveSelection(app.localDriveSelection);
}

void setLocalDriveSelection(LocalDriveSelection selection) {
    app.localDriveSelection = static_cast<int32_t>(selection);
}

String localDriveSelectionLabel(LocalDriveSelection selection) {
    switch(selection) {
        case LOCAL_DRIVE_JMC: return "JMC";
        case LOCAL_DRIVE_RT: return "RT";
        case LOCAL_DRIVE_AUTO:
        default:
            return "AUTO";
    }
}

bool isRadioModeItem(const SettingItem& item) {
    return item.menuNum == 1 && item.itemIdx == RADIOMODE;
}

bool isMotorDirectionItem(const SettingItem& item) {
    return item.menuNum == 1 && item.itemIdx == SETMOTORDIRECTION;
}

bool isEspNowPeerSlotItem(const SettingItem& item) {
    return item.action != nullptr && strcmp(item.action, "peer_slot") == 0;
}

bool isEspNowPeerScanItem(const SettingItem& item) {
    return item.action != nullptr && strcmp(item.action, "peer_scan") == 0;
}

bool isEspNowPeerClearItem(const SettingItem& item) {
    return item.action != nullptr && strcmp(item.action, "peer_clear") == 0;
}

bool isControllerLinkItem(const SettingItem& item) {
    return item.action != nullptr &&
           strcmp(item.action, "screen") == 0 &&
           item.actionArg != nullptr &&
           strcmp(item.actionArg, "peers") == 0;
}

bool macAddressEqual(const uint8_t* lhs, const uint8_t* rhs) {
    return lhs != nullptr && rhs != nullptr && memcmp(lhs, rhs, 6) == 0;
}

String formatMacAddress(const uint8_t* mac) {
    if(mac == nullptr) {
        return "--:--:--:--:--:--";
    }

    char buffer[18] = {};
    snprintf(buffer,
             sizeof(buffer),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    return String(buffer);
}

String shortMacAddress(const uint8_t* mac) {
    if(mac == nullptr) {
        return "--:--:--";
    }

    char buffer[9] = {};
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    return String(buffer);
}

bool parseMacAddress(const String& text, uint8_t* macOut) {
    if(macOut == nullptr) {
        return false;
    }

    unsigned values[6] = {};
    if(sscanf(text.c_str(),
              "%2x:%2x:%2x:%2x:%2x:%2x",
              &values[0],
              &values[1],
              &values[2],
              &values[3],
              &values[4],
              &values[5]) != 6) {
        return false;
    }

    for(uint8_t i = 0; i < 6; ++i) {
        macOut[i] = static_cast<uint8_t>(values[i] & 0xFFu);
    }
    return true;
}

const uint8_t* activeEspNowPeerMac() {
    if(app.espNowPeerConfigured) {
        return app.espNowPeerMac;
    }
    return remote_config::kTargetMacConfigured ? remote_config::kTargetMac : nullptr;
}

bool hasActiveEspNowPeer() {
    return activeEspNowPeerMac() != nullptr;
}

bool lockedEspNowPeerMatches(const uint8_t* mac) {
    const uint8_t* activeMac = activeEspNowPeerMac();
    return activeMac != nullptr && macAddressEqual(activeMac, mac);
}

void clearLockedEspNowPeer() {
    app.espNowPeerConfigured = false;
    memset(app.espNowPeerMac, 0, sizeof(app.espNowPeerMac));
}

void setLockedEspNowPeer(const uint8_t* mac) {
    if(mac == nullptr) {
        clearLockedEspNowPeer();
        return;
    }

    memcpy(app.espNowPeerMac, mac, sizeof(app.espNowPeerMac));
    app.espNowPeerConfigured = true;
}

void expireDiscoveredEspNowPeers() {
    const uint32_t now = millis();
    for(uint8_t i = 0; i < kEspNowPeerSlots; ++i) {
        if(app.espNowPeers[i].valid &&
           (now - app.espNowPeers[i].seenAt) > kEspNowDiscoveryExpireMs) {
            app.espNowPeers[i] = {};
        }
    }
}

const DiscoveredEspNowPeer* visibleEspNowPeerAt(uint8_t visibleIndex) {
    expireDiscoveredEspNowPeers();
    uint8_t seen = 0;
    for(uint8_t i = 0; i < kEspNowPeerSlots; ++i) {
        if(!app.espNowPeers[i].valid) {
            continue;
        }
        if(seen == visibleIndex) {
            return &app.espNowPeers[i];
        }
        ++seen;
    }
    return nullptr;
}

uint8_t visibleEspNowPeerCount() {
    expireDiscoveredEspNowPeers();
    uint8_t count = 0;
    for(uint8_t i = 0; i < kEspNowPeerSlots; ++i) {
        if(app.espNowPeers[i].valid) {
            ++count;
        }
    }
    return count;
}

String espNowPeerName(const DiscoveredEspNowPeer& peer) {
    if(peer.name.length() > 0) {
        return peer.name;
    }
    if(peer.target.length() > 0) {
        return peer.target;
    }
    return String("Controller ") + shortMacAddress(peer.mac);
}

String espNowPeerLabelForItem(const SettingItem& item) {
    if(isEspNowPeerScanItem(item)) {
        return "SCAN AGAIN";
    }
    if(isEspNowPeerClearItem(item)) {
        return "FORGET LOCK";
    }

    const DiscoveredEspNowPeer* peer = visibleEspNowPeerAt(static_cast<uint8_t>(item.itemIdx));
    if(peer == nullptr) {
        return "NO DEVICE";
    }
    return upperText(espNowPeerName(*peer));
}

bool isConfirmActionItem(const SettingItem& item) {
    if(item.action == nullptr) {
        return false;
    }
    return strcmp(item.action, "calibrate") == 0 ||
           strcmp(item.action, "reset_network") == 0 ||
           strcmp(item.action, "reset_defaults") == 0;
}

bool usesChoiceEditor(const SettingItem& item) {
    return item.kind == ItemKind::Toggle || isConfirmActionItem(item);
}

String toggleOffLabel(const SettingItem& item) {
    if(isInvertScrollItem(item)) {
        return "CLOCKWISE";
    }
    if(isUiSoundsItem(item)) {
        return "MUTE";
    }
    if(isConfirmActionItem(item)) {
        return "NO";
    }
    if(isRadioModeItem(item)) {
        return "WI-FI";
    }
    if(isMotorDirectionItem(item)) {
        return "CLOCKWISE";
    }
    return "OFF";
}

String toggleOnLabel(const SettingItem& item) {
    if(isInvertScrollItem(item)) {
        return "COUNTER-CLOCKWISE";
    }
    if(isUiSoundsItem(item)) {
        return "ON";
    }
    if(isConfirmActionItem(item)) {
        return "YES";
    }
    if(isRadioModeItem(item)) {
        return "ESP-NOW";
    }
    if(isMotorDirectionItem(item)) {
        return "COUNTER-CLOCKWISE";
    }
    return "ON";
}

void applyBacklightLevel(uint8_t level, bool force = false) {
    if(!force && g_backlightLevelValid && g_appliedBacklightLevel == level) {
        return;
    }

    remote_board_set_backlight(level);
    g_appliedBacklightLevel = level;
    g_backlightLevelValid = true;
}

void applyLocalMenuPreview(const SettingItem& item, int32_t value) {
    if(isDisplayBrightnessItem(item)) {
        applyBacklightLevel(static_cast<uint8_t>(clampValue(value, 1, 255)), true);
    }
}

void applyCurrentBacklightLevel(bool force = false) {
    applyBacklightLevel(currentBacklightLevel(), force);
}

void persistStatusFrame(PersistedStatusFrame* out, const sg98_protocol::StatusFrame& status) {
    if(out == nullptr) {
        return;
    }

    copyStringToBuffer(status.state, out->state);
    copyStringToBuffer(status.drive, out->drive);
    copyStringToBuffer(status.comm, out->comm);
    copyStringToBuffer(status.motor, out->motor);
    copyStringToBuffer(status.scale, out->scale);
    out->error = status.error;
    out->setRpm = status.setRpm;
    out->motorSetRpm = status.motorSetRpm;
    out->currentRpm = status.currentRpm;
    out->torquePercent = status.torquePercent;
    out->setWeightMg = status.setWeightMg;
    out->currentWeightMg = status.currentWeightMg;
    out->currentMenu = status.currentMenu;
    out->enterMenu = status.enterMenu;
    out->jmcBaud = status.jmcBaud;
    out->preferredJmcBaud = status.preferredJmcBaud;
}

void restoreStatusFrame(const PersistedStatusFrame& in, sg98_protocol::StatusFrame* out) {
    if(out == nullptr) {
        return;
    }

    out->state = String(in.state);
    out->drive = String(in.drive);
    out->comm = String(in.comm);
    out->motor = String(in.motor);
    out->scale = String(in.scale);
    out->error = in.error;
    out->setRpm = in.setRpm;
    out->motorSetRpm = in.motorSetRpm;
    out->currentRpm = in.currentRpm;
    out->torquePercent = in.torquePercent;
    out->setWeightMg = in.setWeightMg;
    out->currentWeightMg = in.currentWeightMg;
    out->currentMenu = in.currentMenu;
    out->enterMenu = in.enterMenu;
    out->jmcBaud = in.jmcBaud;
    out->preferredJmcBaud = in.preferredJmcBaud;
}

void captureSnapshot(SleepSnapshot* snapshot, bool includeStatus) {
    if(snapshot == nullptr) {
        return;
    }

    snapshot->magic = kSleepSnapshotMagic;
    snapshot->version = kSleepSnapshotVersion;
    snapshot->screen = static_cast<uint8_t>(app.screen);
    memcpy(snapshot->selection, app.selection, sizeof(snapshot->selection));
    snapshot->manualRpm = app.manualRpm;
    snapshot->weightMg = app.weightMg;
    snapshot->localInvertScroll = app.localInvertScroll;
    snapshot->localUiSounds = app.localUiSounds;
    snapshot->localDriveSelection = app.localDriveSelection;
    memcpy(snapshot->menuValues, app.menuValues, sizeof(snapshot->menuValues));
    snapshot->menuSyncedMask = 0;
    for(uint8_t menu = 0; menu < 5; ++menu) {
        if(app.menuSynced[menu]) {
            snapshot->menuSyncedMask |= static_cast<uint8_t>(1u << menu);
        }
    }
    const LinkTransport persistedTransport = app.currentTransport == LinkTransport::None ? initialTransport() : app.currentTransport;
    snapshot->transport = static_cast<uint8_t>(persistedTransport);
    snapshot->preferGbw = app.preferGbw;
    snapshot->radioModeKnown = app.radioModeKnown;
    snapshot->espNowPeerConfigured = app.espNowPeerConfigured;
    memcpy(snapshot->espNowPeerMac, app.espNowPeerMac, sizeof(snapshot->espNowPeerMac));
    snapshot->hasStatus = includeStatus && app.hasStatus;
    if(includeStatus && app.hasStatus) {
        persistStatusFrame(&snapshot->status, app.status);
    } else {
        snapshot->status = {};
    }
}

void restoreSnapshot(const SleepSnapshot& snapshot, LinkTransport* restoredTransport, const char* toastText = nullptr) {
    const uint8_t screenIndex = snapshot.screen;
    if(screenIndex <= static_cast<uint8_t>(Screen::Network)) {
        app.screen = static_cast<Screen>(screenIndex);
    }
    memcpy(app.selection, snapshot.selection, sizeof(app.selection));
    sanitizeSelections();
    app.manualRpm = snapshot.manualRpm;
    app.weightMg = snapshot.weightMg;
    app.localInvertScroll = clampValue(snapshot.localInvertScroll, 0, 1);
    app.localUiSounds = clampValue(snapshot.localUiSounds, 0, 1);
    if(snapshot.version <= kSleepSnapshotVersionLegacyUiSoundsOff && app.localUiSounds == 0) {
        app.localUiSounds = 1;
    }
    setLocalDriveSelection(sanitizeLocalDriveSelection(snapshot.localDriveSelection));
    memcpy(app.menuValues, snapshot.menuValues, sizeof(app.menuValues));
    for(uint8_t menu = 0; menu < 5; ++menu) {
        app.menuSynced[menu] = (snapshot.menuSyncedMask & static_cast<uint8_t>(1u << menu)) != 0;
    }
    app.preferGbw = snapshot.preferGbw;
    app.radioModeKnown = snapshot.radioModeKnown;
    app.espNowPeerConfigured = snapshot.espNowPeerConfigured;
    memcpy(app.espNowPeerMac, snapshot.espNowPeerMac, sizeof(app.espNowPeerMac));
    app.hasStatus = snapshot.hasStatus;
    if(app.hasStatus) {
        restoreStatusFrame(snapshot.status, &app.status);
        app.lastStatusAt = millis();
    }
    if(restoredTransport != nullptr) {
        *restoredTransport = sanitizeTransport(snapshot.transport);
    }
    if(toastText != nullptr && toastText[0] != '\0') {
        setToast(toastText, remote_config::kWakeToastMs);
    }
    app.redraw = true;
    app.lastUserActivityAt = millis();
    app.lastControlActivityAt = app.lastUserActivityAt;
    app.motionHomeCandidateAt = 0;
    app.lastMotionHomeAt = 0;
}

bool wakeStatusIncludesGpio(int8_t gpioNum) {
    if(gpioNum < 0) {
        return false;
    }

    const uint64_t wakeBit = uint64_t{1} << static_cast<unsigned>(gpioNum);
    switch(esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT1:
            return (esp_sleep_get_ext1_wakeup_status() & wakeBit) != 0;
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        case ESP_SLEEP_WAKEUP_GPIO:
            return (esp_sleep_get_gpio_wakeup_status() & wakeBit) != 0;
#endif
        default:
            return false;
    }
}

bool wokeFromMotionSource() {
    return remote_board_wake_supported(RemoteWakeSource::Motion) &&
           wakeStatusIncludesGpio(remote_board_pins().imuInt);
}

bool returnToHomeOnMotion(bool markPersistentState) {
    if(app.screen == Screen::Home && !app.editing) {
        return false;
    }

    app.screen = Screen::Home;
    app.menuRollOffset = 0.0f;
    app.editing = false;
    app.redraw = true;
    touchTracker = {};
    app.motionHomeCandidateAt = 0;
    app.lastMotionHomeAt = millis();
    sanitizeSelections();
    if(markPersistentState) {
        markPersistentStateDirty(kPersistentStateDebounceMs);
    }
    return true;
}

void markPersistentStateDirty(uint32_t debounceMs = kPersistentStateDebounceMs) {
    if(!g_statePrefsReady) {
        return;
    }
    g_persistentStateDirty = true;
    g_persistentStateDueAt = millis() + debounceMs;
}

void savePersistentSnapshotNow() {
    if(!g_statePrefsReady) {
        return;
    }

    SleepSnapshot snapshot = {};
    captureSnapshot(&snapshot, false);
    if(g_savedPersistentSnapshotValid &&
       memcmp(&g_savedPersistentSnapshot, &snapshot, sizeof(snapshot)) == 0) {
        g_persistentStateDirty = false;
        return;
    }

    const size_t written = g_statePrefs.putBytes(kPersistentStateKey, &snapshot, sizeof(snapshot));
    if(written == sizeof(snapshot)) {
        g_savedPersistentSnapshot = snapshot;
        g_savedPersistentSnapshotValid = true;
        g_persistentStateDirty = false;
    }
}

void servicePersistentSnapshotSave() {
    if(!g_persistentStateDirty) {
        return;
    }
    if(static_cast<int32_t>(millis() - g_persistentStateDueAt) < 0) {
        return;
    }
    savePersistentSnapshotNow();
}

bool restorePersistentSnapshot(LinkTransport* restoredTransport) {
    if(!g_statePrefsReady) {
        return false;
    }

    SleepSnapshot snapshot = {};
    const size_t read = g_statePrefs.getBytes(kPersistentStateKey, &snapshot, sizeof(snapshot));
    if(read != sizeof(snapshot) ||
       snapshot.magic != kSleepSnapshotMagic ||
       (snapshot.version != kSleepSnapshotVersion &&
        snapshot.version != kSleepSnapshotVersionLegacyUiSoundsOff)) {
        return false;
    }

    restoreSnapshot(snapshot, restoredTransport);
    g_savedPersistentSnapshot = snapshot;
    g_savedPersistentSnapshotValid = true;
    g_persistentStateDirty = false;
    return true;
}

void saveSleepSnapshot() {
    captureSnapshot(&g_sleepSnapshot, true);
}

bool restoreSleepSnapshot(LinkTransport* restoredTransport) {
    if(g_sleepSnapshot.magic != kSleepSnapshotMagic ||
       (g_sleepSnapshot.version != kSleepSnapshotVersion &&
        g_sleepSnapshot.version != kSleepSnapshotVersionLegacyUiSoundsOff) ||
       esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        return false;
    }

    restoreSnapshot(g_sleepSnapshot, restoredTransport, "Awake");
    if(wokeFromMotionSource()) {
        returnToHomeOnMotion(false);
    }
    return true;
}

bool addDeepSleepWakePin(uint64_t* mask, int8_t gpioNum, RemoteWakeSource source) {
    if(mask == nullptr || gpioNum < 0 || !remote_board_wake_supported(source)) {
        return false;
    }

    const gpio_num_t gpio = static_cast<gpio_num_t>(gpioNum);
    if(!esp_sleep_is_valid_wakeup_gpio(gpio)) {
        return false;
    }

    *mask |= uint64_t{1} << static_cast<unsigned>(gpioNum);
    return true;
}

uint64_t deepSleepWakeMask() {
    const RemoteBoardPins& pins = remote_board_pins();
    uint64_t mask = 0;
    addDeepSleepWakePin(&mask, pins.touchInt, RemoteWakeSource::Touch);
    addDeepSleepWakePin(&mask, pins.primaryButton, RemoteWakeSource::PrimaryButton);
    addDeepSleepWakePin(&mask, pins.encoderSwitch, RemoteWakeSource::EncoderSwitch);
    addDeepSleepWakePin(&mask, pins.imuInt, RemoteWakeSource::Motion);
    return mask;
}

bool idleDeepSleepSupported() {
    return remote_board_support_present(remote_board_feature_map().power.deepSleep) &&
           deepSleepWakeMask() != 0;
}

void debugRoundInput(const char* tag, int32_t a = 0, int32_t b = 0, int32_t c = 0, int32_t d = 0) {
    if(!kRoundInputDebug || !useRoundNavigation()) {
        return;
    }

    const uint32_t now = millis();
    if(strcmp(tag, "touch_move") == 0 && (now - app.lastRoundInputDebugAt) < 60) {
        return;
    }
    app.lastRoundInputDebugAt = now;
    Serial.printf("[round] %s a=%ld b=%ld c=%ld d=%ld screen=%u edit=%u target=%u\n",
                  tag,
                  static_cast<long>(a),
                  static_cast<long>(b),
                  static_cast<long>(c),
                  static_cast<long>(d),
                  static_cast<unsigned>(app.screen),
                  app.editing ? 1u : 0u,
                  app.targetDirty ? 1u : 0u);
}

bool roundUiInteractionActive() {
    if(!useRoundNavigation()) {
        return false;
    }

    const uint32_t now = millis();
    return touchTracker.active || app.targetDirty || app.editing ||
           (now - app.lastUserActivityAt) < kRoundInputQuietMs;
}

bool shouldYieldTransportToUi() {
    if(kDisableTransportPerfTest || !useRoundNavigation()) {
        return false;
    }

    if(touchTracker.active || app.targetDirty) {
        return true;
    }

    if(pendingControllerSyncActive()) {
        return false;
    }

    if(app.currentTransport == LinkTransport::WebSocket ||
       app.pendingTransport == LinkTransport::WebSocket ||
       remote_wifi_is_active()) {
        return false;
    }

    if(app.screen != Screen::Home || app.editing) {
        return false;
    }

    return (millis() - app.lastUserActivityAt) < kHomeTransportQuietMs;
}

uint32_t currentStatusPollIntervalMs() {
    if((app.pendingTarget.active || app.pendingAction.kind != PendingActionKind::None) &&
       !touchTracker.active) {
        return remote_config::kStatusPollInteractiveMs;
    }

    if(useRoundNavigation() && (app.screen != Screen::Home || app.editing)) {
        return remote_config::kStatusPollRoundMenuMs;
    }

    if(useRoundNavigation() && app.screen == Screen::Home) {
        if(roundUiInteractionActive()) {
            return kHomeStatusPollActiveMs;
        }

        if(isRunningState()) {
            return kHomeStatusPollRunningMs;
        }

        return kHomeStatusPollIdleMs;
    }

    if(roundUiInteractionActive()) {
        return remote_config::kStatusPollRoundInputMs;
    }

    if(isRunningState()) {
        return remote_config::kStatusPollRunningMs;
    }

    if(app.targetDirty || (millis() - app.lastUserActivityAt) < remote_config::kStatusPollBoostMs) {
        return remote_config::kStatusPollInteractiveMs;
    }

    return remote_config::kStatusPollIdleMs;
}

void markUserActivity() {
    app.lastUserActivityAt = millis();
}

bool handleMotionHomeGesture(uint32_t now) {
    if(app.screen == Screen::Home && !app.editing) {
        app.motionHomeCandidateAt = 0;
        return false;
    }

    if((now - app.lastMotionHomeAt) < kMotionHomeCooldownMs) {
        return false;
    }

    if((now - app.lastControlActivityAt) < kMotionHomeIgnoreAfterControlMs) {
        app.motionHomeCandidateAt = 0;
        return false;
    }

    if(app.motionHomeCandidateAt == 0 ||
       (now - app.motionHomeCandidateAt) > kMotionHomeConfirmWindowMs) {
        app.motionHomeCandidateAt = now;
        setToast("Shake again for RUN", 900);
        return false;
    }

    if((now - app.motionHomeCandidateAt) < kMotionHomeMinGapMs) {
        return false;
    }

    return returnToHomeOnMotion(true);
}

bool stateAllowsModeToggle() {
    return !app.hasStatus || app.status.state == "IDLE" || app.status.state == "IDLE_GBW";
}

int applyDialDirection(int delta) {
    if(delta == 0) {
        return 0;
    }
    return -delta;
}

bool isVisibleControllerMenuScreen(Screen screen, uint8_t menuNum) {
    switch(menuNum) {
        case 1: return screen == Screen::Main || screen == Screen::Gbw || screen == Screen::Network ||
                       screen == Screen::AudioDiag;
        case 2: return screen == Screen::Purge;
        case 3: return screen == Screen::Gbw;
        default: return false;
    }
}

bool statusFrameEqual(const sg98_protocol::StatusFrame& lhs, const sg98_protocol::StatusFrame& rhs) {
    return lhs.state == rhs.state && lhs.drive == rhs.drive && lhs.comm == rhs.comm && lhs.motor == rhs.motor &&
           lhs.scale == rhs.scale && lhs.error == rhs.error && lhs.setRpm == rhs.setRpm &&
           lhs.motorSetRpm == rhs.motorSetRpm && lhs.currentRpm == rhs.currentRpm &&
           lhs.torquePercent == rhs.torquePercent && lhs.setWeightMg == rhs.setWeightMg &&
           lhs.currentWeightMg == rhs.currentWeightMg && lhs.currentMenu == rhs.currentMenu &&
           lhs.enterMenu == rhs.enterMenu && lhs.jmcBaud == rhs.jmcBaud &&
           lhs.preferredJmcBaud == rhs.preferredJmcBaud;
}

bool menuFrameMatchesCache(const sg98_protocol::MenuFrame& frame) {
    if(frame.menuNum >= 5 || !app.menuSynced[frame.menuNum]) {
        return false;
    }

    for(uint8_t i = 0; i < frame.count && i < sg98_protocol::kMaxMenuValues; ++i) {
        if(app.menuValues[frame.menuNum][i] != frame.values[i]) {
            return false;
        }
    }
    return true;
}

bool shouldRedrawForStatusFrame(const sg98_protocol::StatusFrame& previous,
                               const sg98_protocol::StatusFrame& status,
                               bool hadStatus,
                               bool valuesChanged) {
    if(!valuesChanged && hadStatus) {
        return false;
    }

    if(!useRoundNavigation()) {
        return true;
    }

    if(app.screen != Screen::Home) {
        return false;
    }

    if(!hadStatus) {
        return true;
    }

    if(roundUiInteractionActive()) {
        return status.state != previous.state || status.error != previous.error;
    }

    if(status.state == "GRINDING" || status.state == "GRINDING_GBW" || status.state == "PURGING") {
        return status.state != previous.state || status.error != previous.error ||
               status.currentRpm != previous.currentRpm ||
               status.currentWeightMg != previous.currentWeightMg ||
               status.setRpm != previous.setRpm ||
               status.setWeightMg != previous.setWeightMg ||
               status.motor != previous.motor || status.drive != previous.drive;
    }

    return status.state != previous.state || status.error != previous.error ||
           status.motor != previous.motor || status.drive != previous.drive ||
           status.comm != previous.comm;
}

bool shouldRedrawForMenuFrame(const sg98_protocol::MenuFrame& frame, bool valuesChanged) {
    if(!valuesChanged) {
        return false;
    }

    if(!useRoundNavigation()) {
        return true;
    }

    if(app.editing) {
        return false;
    }

    return isVisibleControllerMenuScreen(app.screen, frame.menuNum);
}

void setToast(const String& text, uint32_t durationMs = remote_config::kToastHoldMs) {
    app.toast = text;
    app.toastUntil = millis() + durationMs;
    app.redraw = true;
}

void refreshBatteryStatus(bool force) {
    if(kDisableTransportPerfTest) {
        return;
    }

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
        case Screen::Network:
            if(count != nullptr) *count = arrayCount(kNetworkItems);
            return kNetworkItems;
        case Screen::AudioDiag:
            if(count != nullptr) *count = arrayCount(kAudioItems);
            return kAudioItems;
        case Screen::Drive:
            if(count != nullptr) *count = arrayCount(kDriveItems);
            return kDriveItems;
        case Screen::EspNowPeers: {
            size_t peerCount = visibleEspNowPeerCount();
            const SettingItem* peerItems = kEspNowPeerItems;
            size_t total = peerCount + 1;
            if(app.espNowPeerConfigured) {
                total += 1;
            }
            if(peerCount == 0) {
                peerItems = kEspNowPeerItems + kEspNowPeerSlots;
                total = app.espNowPeerConfigured ? 2 : 1;
            }
            if(total > arrayCount(kEspNowPeerItems)) {
                total = arrayCount(kEspNowPeerItems);
            }
            if(count != nullptr) *count = total;
            return peerItems;
        }
        case Screen::Home:
        default:
            if(count != nullptr) *count = 0;
            return nullptr;
    }
}

uint8_t& selectionFor(Screen screen) {
    return app.selection[static_cast<uint8_t>(screen)];
}

void sanitizeSelections() {
    app.selection[static_cast<uint8_t>(Screen::Home)] = 0;
    for(const Screen screen : kMenuScreens) {
        size_t count = 0;
        itemsForScreen(screen, &count);
        if(count == 0) {
            selectionFor(screen) = 0;
            continue;
        }
        uint8_t& selected = selectionFor(screen);
        if(selected >= count) {
            selected = static_cast<uint8_t>(count - 1);
        }
    }

    size_t driveCount = 0;
    itemsForScreen(Screen::Drive, &driveCount);
    if(driveCount == 0) {
        selectionFor(Screen::Drive) = 0;
    } else if(selectionFor(Screen::Drive) >= driveCount) {
        selectionFor(Screen::Drive) = static_cast<uint8_t>(driveCount - 1);
    }

    size_t espNowPeerCount = 0;
    itemsForScreen(Screen::EspNowPeers, &espNowPeerCount);
    if(espNowPeerCount == 0) {
        selectionFor(Screen::EspNowPeers) = 0;
    } else if(selectionFor(Screen::EspNowPeers) >= espNowPeerCount) {
        selectionFor(Screen::EspNowPeers) = static_cast<uint8_t>(espNowPeerCount - 1);
    }
}

int32_t menuValue(const SettingItem& item) {
    if(isInvertScrollItem(item)) {
        return app.localInvertScroll;
    }
    if(isUiSoundsItem(item)) {
        return app.localUiSounds;
    }
    if(item.menuNum == 0) {
        return 0;
    }
    return app.menuValues[item.menuNum][item.itemIdx];
}

String formatValueLabel(const SettingItem& item, int32_t value) {
    if(usesChoiceEditor(item)) {
        return value != 0 ? toggleOnLabel(item) : toggleOffLabel(item);
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
        if(isEspNowPeerSlotItem(item)) {
            const DiscoveredEspNowPeer* peer = visibleEspNowPeerAt(static_cast<uint8_t>(item.itemIdx));
            if(peer == nullptr) {
                return "--";
            }
            return lockedEspNowPeerMatches(peer->mac) ? "ACTIVE" : shortMacAddress(peer->mac);
        }
        if(isEspNowPeerScanItem(item)) {
            return "RUN";
        }
        if(isEspNowPeerClearItem(item)) {
            return app.espNowPeerConfigured ? "CLEAR" : "--";
        }
        if(isControllerLinkItem(item)) {
            if(controllerLinkUsesWebSocketScan()) {
                if(!remote_wifi_has_saved_network()) {
                    return "SET WI-FI";
                }
                if(app.wsConnected) {
                    return "LINKED";
                }
                if(app.wsClientStarted) {
                    return "TRYING";
                }
                return "SCAN";
            }
            return hasActiveEspNowPeer() ? "OPEN" : "SCAN";
        }
        if(isConfirmActionItem(item)) {
            return "CONFIRM";
        }
        if(item.action != nullptr && strcmp(item.action, "screen") == 0) {
            if(item.actionArg != nullptr && strcmp(item.actionArg, "drive") == 0) {
                return localDriveSelectionLabel(currentLocalDriveSelection());
            }
            return "OPEN";
        }
        if(item.action != nullptr && strcmp(item.action, "redetect") == 0) {
            return currentLocalDriveSelection() == LOCAL_DRIVE_AUTO ? "ACTIVE" : "RUN";
        }
        if(item.action != nullptr && strcmp(item.action, "drive") == 0) {
            if(item.actionArg != nullptr) {
                if(currentLocalDriveSelection() == LOCAL_DRIVE_JMC && strcmp(item.actionArg, "jmc") == 0) {
                    return "ACTIVE";
                }
                if(currentLocalDriveSelection() == LOCAL_DRIVE_RT && strcmp(item.actionArg, "rt") == 0) {
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
    app.localInvertScroll = 0;
    app.localUiSounds = 1;
    setLocalDriveSelection(LOCAL_DRIVE_AUTO);
    app.menuValues[1][INVERT_SCROLL] = 0;
    app.menuValues[1][SETMAX] = default_maxRPM;
    app.menuValues[1][SETMIN] = default_minRPM;
    app.menuValues[1][SETMOTORTORQUE] = default_motor_torque;
    app.menuValues[1][SETMOTORRAMP] = default_motor_ramp;
    app.menuValues[1][SETMOTORDIRECTION] = default_motor_direction_reversed;

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
    const bool largeCanvas = isLargeCanvas();
    layout.pad = clampI16(shortSide / 24, 10, 20);
    layout.gap = clampI16(shortSide / 40, 6, 12);
    layout.topPillH = largeCanvas ? clampI16(shortSide / 15, 24, 30) : clampI16(shortSide / 13, 22, 34);
    layout.buttonH = largeCanvas ? clampI16(shortSide / 11, 30, 38) : clampI16(shortSide / 10, 24, 42);
    layout.navH = largeCanvas ? clampI16(shortSide / 12, 28, 36) : clampI16(shortSide / 10, 24, 44);

    const int16_t centerX = static_cast<int16_t>(layout.width / 2);
    const int16_t centerY = static_cast<int16_t>(layout.height / 2);
    const float safeRadius = static_cast<float>(shortSide / 2 - (largeCanvas ? 12 : 8));
    const int16_t topInset = static_cast<int16_t>(layout.pad + (largeCanvas ? 12 : 0));
    const int16_t bottomInset = static_cast<int16_t>(layout.pad + (largeCanvas ? 18 : 0));

    auto safeBand = [&](int16_t y, int16_t h, int16_t inset) {
        const float yTop = fabsf(static_cast<float>(y - centerY));
        const float yBottom = fabsf(static_cast<float>((y + h) - centerY));
        const float maxY = max(yTop, yBottom);
        const float safeHalf = maxY >= safeRadius ? 0.0f : sqrtf((safeRadius * safeRadius) - (maxY * maxY));
        int16_t left = static_cast<int16_t>(roundf(static_cast<float>(centerX) - safeHalf)) + inset;
        int16_t right = static_cast<int16_t>(roundf(static_cast<float>(centerX) + safeHalf)) - inset;
        if(right <= left) {
            left = inset;
            right = static_cast<int16_t>(layout.width - inset);
        }
        return Rect(left, y, static_cast<int16_t>(right - left), h);
    };

    if(largeCanvas) {
        const int16_t pillGap = layout.gap;
        const Rect topBand = safeBand(topInset, layout.topPillH, static_cast<int16_t>(layout.pad + 16));
        const int16_t pillW = static_cast<int16_t>((topBand.w - (pillGap * 2)) / 3);
        layout.topLeft = {topBand.x, topInset, pillW, layout.topPillH};
        layout.topCenter = {static_cast<int16_t>(layout.topLeft.x + pillW + pillGap), topInset, pillW, layout.topPillH};
        layout.topRight = {static_cast<int16_t>(layout.topCenter.x + pillW + pillGap), topInset, pillW, layout.topPillH};

        layout.homeDialCx = centerX;
        layout.homeDialCy = static_cast<int16_t>(centerY - 18);
        layout.homeDialOuterRadius = clampI16(static_cast<int16_t>(shortSide * 0.25f), 84, 118);
        layout.homeDialInnerRadius = clampI16(static_cast<int16_t>(layout.homeDialOuterRadius * 0.62f), 52, 74);

        const int16_t modeY =
            static_cast<int16_t>(layout.homeDialCy - layout.homeDialOuterRadius - layout.topPillH - layout.gap - 8);
        const Rect modeBand = safeBand(modeY, static_cast<int16_t>(layout.topPillH + 2), static_cast<int16_t>(layout.pad + 24));
        const int16_t modeW = clampI16(static_cast<int16_t>(modeBand.w * 0.72f), 156, 220);
        layout.modeBadge = {
            static_cast<int16_t>(centerX - (modeW / 2)),
            modeY,
            modeW,
            static_cast<int16_t>(layout.topPillH + 2)
        };

        const int16_t navY = static_cast<int16_t>(layout.height - bottomInset - layout.navH);
        const bool waveshareRoundMenu = useWaveshareRoundUi() && app.screen != Screen::Home && !app.editing;
        const int16_t edgeNavW =
            waveshareRoundMenu ? clampI16(static_cast<int16_t>(shortSide * 0.11f), 46, 56)
                               : clampI16(static_cast<int16_t>(shortSide * 0.19f), 76, 96);
        const int16_t edgeNavH =
            waveshareRoundMenu ? clampI16(static_cast<int16_t>(shortSide * 0.25f), 96, 122)
                               : clampI16(static_cast<int16_t>(shortSide * 0.34f), 132, 176);
        const int16_t edgeNavY = static_cast<int16_t>(centerY - edgeNavH / 2);
        layout.homeModeButton = {layout.pad, edgeNavY, edgeNavW, edgeNavH};
        layout.homeActionButton = {
            static_cast<int16_t>(layout.width - layout.pad - edgeNavW),
            edgeNavY,
            edgeNavW,
            edgeNavH
        };
        layout.navPrev = {layout.pad, edgeNavY, edgeNavW, edgeNavH};
        layout.navHome = {};
        layout.navNext = {
            static_cast<int16_t>(layout.width - layout.pad - edgeNavW),
            edgeNavY,
            edgeNavW,
            edgeNavH
        };

        const Rect footerBand = safeBand(static_cast<int16_t>(navY - layout.topPillH - layout.gap),
                                         static_cast<int16_t>(layout.topPillH - 2),
                                         static_cast<int16_t>(layout.pad + 22));
        layout.footer = {
            footerBand.x,
            footerBand.y,
            footerBand.w,
            static_cast<int16_t>(layout.topPillH - 2)
        };

        layout.editorCx = centerX;
        layout.editorCy = static_cast<int16_t>(centerY - 8);
        layout.editorOuterRadius = clampI16(static_cast<int16_t>(shortSide * 0.24f), 78, 114);
        layout.editorInnerRadius = clampI16(static_cast<int16_t>(layout.editorOuterRadius * 0.63f), 50, 72);
        const int16_t editButtonH = static_cast<int16_t>(layout.buttonH + 8);
        const int16_t editTopY = static_cast<int16_t>(topInset + layout.topPillH + layout.gap + 2);
        const Rect editTopBand = safeBand(editTopY, editButtonH, static_cast<int16_t>(layout.pad + 40));
        layout.editCancel = {editTopBand.x, editTopY, editTopBand.w, editButtonH};
        const int16_t editBottomY = static_cast<int16_t>(layout.height - bottomInset - editButtonH - 10);
        const Rect editBottomBand = safeBand(editBottomY, editButtonH, static_cast<int16_t>(layout.pad + 34));
        layout.editSave = {editBottomBand.x, editBottomY, editBottomBand.w, editButtonH};

        if(app.screen != Screen::Home && !app.editing) {
            if(useWaveshareRoundUi()) {
                const int16_t stepDistance = clampI16(static_cast<int16_t>(shortSide / 7), 56, 68);
                const int16_t rowH = clampI16(static_cast<int16_t>(stepDistance - 6), 50, 64);
                const int16_t rowInset = static_cast<int16_t>(layout.pad + 34);
                const int16_t centerRowY = static_cast<int16_t>(centerY - (rowH / 2));
                layout.menuRows[0] = safeBand(static_cast<int16_t>(centerRowY - stepDistance), rowH, rowInset);
                layout.menuRows[1] = safeBand(centerRowY, rowH, rowInset);
                layout.menuRows[2] = safeBand(static_cast<int16_t>(centerRowY + stepDistance), rowH, rowInset);
                layout.rowH = rowH;
                layout.rowCount = 3;
            } else {
                const int16_t heroH = clampI16(static_cast<int16_t>(shortSide * 0.35f), 146, 176);
                const int16_t peekH = clampI16(static_cast<int16_t>(shortSide * 0.078f), 24, 38);
                const int16_t heroY = static_cast<int16_t>(centerY - (heroH / 2));
                const Rect heroBand = safeBand(heroY, heroH, static_cast<int16_t>(layout.pad + 44));
                const Rect prevBand = safeBand(static_cast<int16_t>(heroY - peekH - layout.gap),
                                               peekH,
                                               static_cast<int16_t>(layout.pad + 88));
                const Rect nextBand = safeBand(static_cast<int16_t>(heroY + heroH + layout.gap),
                                               peekH,
                                               static_cast<int16_t>(layout.pad + 88));
                layout.menuRows[0] = prevBand;
                layout.menuRows[1] = heroBand;
                layout.menuRows[2] = nextBand;
                layout.rowH = heroH;
                layout.rowCount = 3;
            }
        }

        return layout;
    }

    const int16_t tabGap = layout.gap;
    const int16_t tabY = static_cast<int16_t>(layout.height - bottomInset - layout.navH);
    const Rect tabBand = safeBand(tabY, layout.navH, static_cast<int16_t>(layout.pad + (largeCanvas ? 8 : 0)));
    const int16_t tabW = static_cast<int16_t>((tabBand.w - (tabGap * 4)) / 5);
    for(uint8_t i = 0; i < 5; ++i) {
        layout.tabs[i] = {
            static_cast<int16_t>(tabBand.x + i * (tabW + tabGap)),
            tabY,
            tabW,
            layout.navH
        };
    }

    const int16_t pillGap = layout.gap;
    const Rect topBand = safeBand(topInset, layout.topPillH, static_cast<int16_t>(layout.pad + (largeCanvas ? 10 : 0)));
    const int16_t pillW = static_cast<int16_t>((topBand.w - (pillGap * 2)) / 3);
    layout.topLeft = {topBand.x, topInset, pillW, layout.topPillH};
    layout.topCenter = {static_cast<int16_t>(layout.topLeft.x + pillW + pillGap), topInset, pillW, layout.topPillH};
    layout.topRight = {static_cast<int16_t>(layout.topCenter.x + pillW + pillGap), topInset, pillW, layout.topPillH};

    layout.homeDialCx = centerX;
    layout.homeDialCy = static_cast<int16_t>(centerY - (largeCanvas ? 28 : (layout.navH / 4)));
    layout.homeDialOuterRadius = clampI16(static_cast<int16_t>(shortSide * (largeCanvas ? 0.22f : 0.27f)), 60, 126);
    layout.homeDialInnerRadius = clampI16(static_cast<int16_t>(layout.homeDialOuterRadius * 0.62f), 34, 92);

    const int16_t modeY =
        static_cast<int16_t>(layout.homeDialCy - layout.homeDialOuterRadius - layout.topPillH - layout.gap - (largeCanvas ? 10 : 0));
    const Rect modeBand = safeBand(modeY, static_cast<int16_t>(layout.topPillH + 2), static_cast<int16_t>(layout.pad + 14));
    const int16_t modeW = clampI16(static_cast<int16_t>(modeBand.w * (largeCanvas ? 0.72f : 0.82f)), 136, 230);
    layout.modeBadge = {
        static_cast<int16_t>(centerX - (modeW / 2)),
        modeY,
        modeW,
        static_cast<int16_t>(layout.topPillH + 2)
    };

    const int16_t buttonY =
        static_cast<int16_t>(layout.homeDialCy + layout.homeDialOuterRadius - (layout.buttonH / 2) -
                             (largeCanvas ? 18 : 0));
    const Rect buttonBand = safeBand(buttonY, layout.buttonH, static_cast<int16_t>(layout.pad + (largeCanvas ? 8 : 0)));
    const int16_t buttonW = static_cast<int16_t>((buttonBand.w - layout.gap) / 2);
    layout.homeModeButton = {buttonBand.x, buttonY, buttonW, layout.buttonH};
    layout.homeActionButton = {
        static_cast<int16_t>(layout.homeModeButton.x + buttonW + layout.gap),
        buttonY,
        buttonW,
        layout.buttonH
    };

    layout.listTop = static_cast<int16_t>(topInset + layout.topPillH + layout.gap + (largeCanvas ? 22 : 6));
    layout.listBottom = static_cast<int16_t>(layout.height - bottomInset - layout.navH - layout.gap -
                                             (largeCanvas ? 22 : 6));
    layout.visibleRows = largeCanvas ? 4 : 4;
    const int16_t availableRows = static_cast<int16_t>(layout.listBottom - layout.listTop);
    layout.rowH = static_cast<int16_t>((availableRows - ((layout.visibleRows - 1) * layout.gap)) / layout.visibleRows);
    if(layout.rowH < 30) {
        layout.rowH = 30;
    }

    const Rect footerBand = safeBand(static_cast<int16_t>(layout.listBottom - layout.topPillH + 4),
                                     static_cast<int16_t>(layout.topPillH - 4),
                                     static_cast<int16_t>(layout.pad + (largeCanvas ? 10 : layout.pad)));
    layout.footer = {
        footerBand.x,
        footerBand.y,
        footerBand.w,
        static_cast<int16_t>(layout.topPillH - 4)
    };

    layout.editorCx = centerX;
    layout.editorCy = static_cast<int16_t>(centerY - (largeCanvas ? 12 : (layout.navH / 8)));
    layout.editorOuterRadius = clampI16(static_cast<int16_t>(shortSide * (largeCanvas ? 0.24f : 0.29f)), 68, 136);
    layout.editorInnerRadius = clampI16(static_cast<int16_t>(layout.editorOuterRadius * 0.63f), 38, 96);
    const int16_t editButtonY =
        static_cast<int16_t>(layout.height - bottomInset - layout.navH - layout.buttonH - layout.gap);
    const Rect editBand = safeBand(editButtonY, layout.buttonH, static_cast<int16_t>(layout.pad + (largeCanvas ? 8 : 0)));
    const int16_t editButtonW = static_cast<int16_t>((editBand.w - layout.gap) / 2);
    layout.editCancel = {
        editBand.x,
        editButtonY,
        editButtonW,
        layout.buttonH
    };
    layout.editSave = {
        static_cast<int16_t>(layout.editCancel.x + editButtonW + layout.gap),
        layout.editCancel.y,
        editButtonW,
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
            const int16_t rowY = static_cast<int16_t>(layout.listTop + row * (layout.rowH + layout.gap));
            const Rect rowBand = safeBand(rowY, layout.rowH, static_cast<int16_t>(layout.pad + (largeCanvas ? 10 : 0)));
            layout.menuRows[row] = {
                rowBand.x,
                rowY,
                rowBand.w,
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

void updateHomeWheelAnimation() {
    const uint32_t now = millis();
    if(app.homeWheelAnimatedAt == 0) {
        app.homeWheelAnimatedAt = now;
    }

    if(app.screen != Screen::Home || !isRunningState() || roundUiInteractionActive()) {
        app.homeWheelAnimatedAt = now;
        app.homeWheelFrameAt = 0;
        return;
    }

    if(app.homeWheelFrameAt != 0 && (now - app.homeWheelFrameAt) < kWheelAnimationFrameMs) {
        return;
    }

    const uint32_t elapsedMs = now - app.homeWheelAnimatedAt;
    app.homeWheelAnimatedAt = now;
    app.homeWheelFrameAt = now;

    const int8_t direction = homeWheelSpinDirection();
    const float turnsPerSecond = homeWheelTurnsPerSecond();
    if(direction == 0 || turnsPerSecond <= 0.0f) {
        return;
    }

    const float deltaTurns = turnsPerSecond * (static_cast<float>(elapsedMs) / 1000.0f);
    app.homeWheelAngle = normalizeAngle(app.homeWheelAngle +
                                        (static_cast<float>(direction) * deltaTurns * static_cast<float>(2.0 * PI)));
    app.redraw = true;
}

void updateMenuRollAnimation() {
    if(fabsf(app.menuRollOffset) < 0.5f) {
        if(app.menuRollOffset != 0.0f) {
            app.menuRollOffset = 0.0f;
            app.redraw = true;
        }
        return;
    }

    app.menuRollOffset *= 0.72f;
    app.redraw = true;
}

void drawSoftCenteredText(RemoteCanvas& canvas,
                          const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor,
                          uint16_t accentColor,
                          bool subtle = false);
void drawNestCard(RemoteCanvas& canvas,
                  const Rect& rect,
                  uint16_t faceColor,
                  uint16_t borderColor,
                  bool compact = false);

void drawPill(RemoteCanvas& canvas,
              const Rect& rect,
              const String& text,
              uint16_t fill,
              uint16_t textColor) {
    const String label = useRoundNavigation() ? upperText(text) : text;
    const RemoteTextStyle style =
        useRoundNavigation() ? chooseLabelTextStyle(label, rect.w) : pillTextStyle();

    if(useRoundNavigation()) {
        const uint16_t face = blendColor565(kColorPanel, fill, 0.34f);
        const uint16_t border = blendColor565(face, kColorBg, 0.05f);
        drawNestCard(canvas, rect, face, border, true);
        drawSoftCenteredText(canvas,
                             fitTextToWidth(label, rect.w, style),
                             static_cast<int16_t>(rect.x + rect.w / 2),
                             static_cast<int16_t>(rect.y + rect.h / 2),
                             style,
                             textColor,
                             face,
                             fill,
                             true);
        return;
    }

    const int16_t radius = rect.h / 2;
    canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, fill);
    canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, kColorPanelHi);
    canvas.drawCenteredText(fitTextToWidth(label, rect.w, style),
                            rect.x + rect.w / 2,
                            rect.y + rect.h / 2,
                            style,
                            textColor,
                            fill);
}

void drawNestCard(RemoteCanvas& canvas,
                  const Rect& rect,
                  uint16_t faceColor,
                  uint16_t borderColor,
                  bool compact) {
    const int16_t radius = clampI16(static_cast<int16_t>(rect.h / (compact ? 3 : 4)), 12, 30);
    const uint16_t shell = blendColor565(kColorBlack, faceColor, compact ? 0.08f : 0.14f);
    const uint16_t face = blendColor565(kColorPanel, faceColor, compact ? 0.32f : 0.52f);
    const uint16_t inset = blendColor565(face, kColorBlack, 0.14f);
    const uint16_t gloss = blendColor565(kColorText, face, 0.88f);
    const int16_t insetRadius = radius > 2 ? static_cast<int16_t>(radius - 2) : radius;
    const int16_t glossRadius = radius > 6 ? static_cast<int16_t>(radius - 6) : 4;
    int16_t glossHeight = static_cast<int16_t>(rect.h / 5);
    if(glossHeight < 5) {
        glossHeight = 5;
    }

    canvas.fillRoundRect(rect.x, rect.y + 4, rect.w, rect.h, radius, shell);
    canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, face);
    if(rect.w > 4 && rect.h > 4) {
        canvas.fillRoundRect(rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4, insetRadius, inset);
    }
    if(rect.w > 12 && rect.h > 10) {
        canvas.fillRoundRect(rect.x + 8, rect.y + 6, rect.w - 16, glossHeight, glossRadius, gloss);
    }
    canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, borderColor);
    if(rect.w > 6 && rect.h > 6) {
        canvas.drawRoundRect(rect.x + 3,
                             rect.y + 3,
                             rect.w - 6,
                             rect.h - 6,
                             insetRadius,
                             blendColor565(borderColor, kColorText, 0.22f));
    }
}

void drawChoiceAction(RemoteCanvas& canvas,
                      const Rect& rect,
                      const String& text,
                      uint16_t fill,
                      uint16_t textColor,
                      bool primary) {
    const String label = upperText(text);
    const RemoteTextStyle style = chooseLabelTextStyle(label, static_cast<int16_t>(rect.w - 40));
    const uint16_t face = blendColor565(kColorPanel, fill, primary ? 0.28f : 0.12f);
    const uint16_t border = primary ? blendColor565(fill, kColorText, 0.18f)
                                    : blendColor565(face, kColorBg, 0.04f);

    drawNestCard(canvas, rect, face, border, true);
    canvas.fillCircle(static_cast<int16_t>(rect.x + 24),
                      static_cast<int16_t>(rect.y + rect.h / 2),
                      primary ? 10 : 7,
                      primary ? fill : blendColor565(kColorMuted, kColorBg, 0.12f));
    drawSoftCenteredText(canvas,
                         fitTextToWidth(label, static_cast<int16_t>(rect.w - 40), style, 12),
                         static_cast<int16_t>(rect.x + rect.w / 2 + 8),
                         static_cast<int16_t>(rect.y + rect.h / 2),
                         style,
                         textColor,
                         face,
                         fill,
                         primary);
}

void drawDockedChoiceAction(RemoteCanvas& canvas,
                            const Rect& rect,
                            const String& text,
                            uint16_t fill,
                            uint16_t textColor,
                            bool dockLeft,
                            bool enabled) {
    (void)dockLeft;
    const String label = upperText(text);
    const RemoteTextStyle style = chooseLabelTextStyle(label, static_cast<int16_t>(rect.w - 6));
    const uint16_t shellColor = blendColor565(kColorPanel, fill, enabled ? 0.18f : 0.10f);
    if(useRoundNavigation() && rect.h > static_cast<int16_t>(rect.w * 2)) {
        const int16_t radius = clampI16(static_cast<int16_t>(rect.w / 2), 18, 28);
        const uint16_t faceColor = blendColor565(kColorBg, fill, enabled ? 0.14f : 0.08f);
        const uint16_t edgeColor = blendColor565(faceColor, kColorBg, 0.08f);
        const uint16_t insetColor = blendColor565(faceColor, kColorBlack, 0.08f);

        canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, faceColor);
        canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, edgeColor);
        if(rect.w > 6 && rect.h > 6) {
            canvas.drawRoundRect(rect.x + 2,
                                 rect.y + 2,
                                 rect.w - 4,
                                 rect.h - 4,
                                 clampI16(static_cast<int16_t>(radius - 3), 12, 24),
                                 blendColor565(edgeColor, kColorText, 0.12f));
        }
        if(rect.w > 10 && rect.h > 10) {
            canvas.fillRoundRect(rect.x + 4,
                                 rect.y + 4,
                                 rect.w - 8,
                                 rect.h - 8,
                                 clampI16(static_cast<int16_t>(radius - 5), 10, 20),
                                 insetColor);
        }
        const RemoteTextStyle verticalStyle = chooseLabelTextStyle(label, static_cast<int16_t>(rect.h - 18));
        const int16_t targetSpan = clampI16(static_cast<int16_t>(rect.h - 72), 72, 110);
        const int16_t glyphGap = label.length() > 1
                                     ? static_cast<int16_t>(targetSpan / static_cast<int16_t>(label.length() - 1))
                                     : 0;
        const int16_t startY = static_cast<int16_t>(rect.y + rect.h / 2 - targetSpan / 2);
        for(size_t i = 0; i < label.length(); ++i) {
            drawSoftCenteredText(canvas,
                                 String(label.charAt(static_cast<unsigned int>(i))),
                                 static_cast<int16_t>(rect.x + rect.w / 2),
                                 static_cast<int16_t>(startY + static_cast<int16_t>(i) * glyphGap),
                                 verticalStyle,
                                 textColor,
                                 insetColor,
                                 faceColor,
                                 true);
        }
        return;
    }

    const uint16_t borderColor = blendColor565(shellColor, kColorBg, 0.08f);
    drawNestCard(canvas, rect, shellColor, borderColor, true);
    drawSoftCenteredText(canvas,
                         fitTextToWidth(label, static_cast<int16_t>(rect.w - 8), style, 8),
                         static_cast<int16_t>(rect.x + rect.w / 2),
                         static_cast<int16_t>(rect.y + rect.h / 2),
                         style,
                         textColor,
                         shellColor,
                         fill,
                         true);
}

void drawSoftCenteredText(RemoteCanvas& canvas,
                          const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor,
                          uint16_t accentColor,
                          bool subtle) {
    if(text.length() == 0) {
        return;
    }

    const uint16_t shadowColor = blendColor565(bgColor, kColorBlack, subtle ? 0.40f : 0.52f);
    const uint16_t outlineColor = blendColor565(accentColor, bgColor, subtle ? 0.48f : 0.36f);
    const uint16_t glowColor = blendColor565(textColor, accentColor, subtle ? 0.72f : 0.56f);

    canvas.drawCenteredText(text, x, static_cast<int16_t>(y + 2), style, shadowColor, bgColor);
    canvas.drawCenteredText(text, static_cast<int16_t>(x - 1), y, style, outlineColor, bgColor);
    canvas.drawCenteredText(text, static_cast<int16_t>(x + 1), y, style, outlineColor, bgColor);
    canvas.drawCenteredText(text, x, static_cast<int16_t>(y - 1), style, glowColor, bgColor);
    canvas.drawCenteredText(text, x, y, style, textColor, bgColor);
}

void drawSoftCenteredTextRotated(RemoteCanvas& canvas,
                                 const String& text,
                                 int16_t x,
                                 int16_t y,
                                 float angleDegrees,
                                 RemoteTextStyle style,
                                 uint16_t textColor,
                                 uint16_t bgColor,
                                 uint16_t accentColor,
                                 bool subtle) {
    if(text.length() == 0) {
        return;
    }

    const uint16_t shadowColor = blendColor565(bgColor, kColorBlack, subtle ? 0.34f : 0.44f);
    const uint16_t glowColor = blendColor565(textColor, accentColor, subtle ? 0.80f : 0.64f);

    canvas.drawCenteredTextRotated(text,
                                   x,
                                   static_cast<int16_t>(y + 1),
                                   angleDegrees,
                                   style,
                                   shadowColor,
                                   bgColor);
    canvas.drawCenteredTextRotated(text, x, y, angleDegrees, style, glowColor, bgColor);
    canvas.drawCenteredTextRotated(text, x, y, angleDegrees, style, textColor, bgColor);
}

void drawArcTextSimple(RemoteCanvas& canvas,
                       const String& text,
                       int16_t centerX,
                       int16_t centerY,
                       int16_t radius,
                       float centerDegrees,
                       float spanDegrees,
                       bool reverse,
                       RemoteTextStyle style,
                       uint16_t textColor,
                       uint16_t bgColor) {
    if(text.length() == 0 || radius <= 0 || spanDegrees <= 0.0f) {
        return;
    }

    const int16_t charWidth = max<int16_t>(1, static_cast<int16_t>(6 * textScale(style)));
    const float charStepDegrees = max(4.0f,
                                      min(18.0f,
                                          static_cast<float>(charWidth + 2) * 57.2957795f /
                                              static_cast<float>(radius)));
    const float maxSpan = max(0.0f, spanDegrees - charStepDegrees);
    const float totalSpan = min(maxSpan, charStepDegrees * static_cast<float>(text.length() - 1));
    const float startDegrees = reverse ? (centerDegrees + (totalSpan * 0.5f)) : (centerDegrees - (totalSpan * 0.5f));
    const float stepDegrees = reverse ? -charStepDegrees : charStepDegrees;

    for(size_t i = 0; i < text.length(); ++i) {
        const float angleDegrees = startDegrees + (stepDegrees * static_cast<float>(i));
        const float angleRadians = angleDegrees * 0.01745329252f;
        const int16_t x = static_cast<int16_t>(roundf(centerX + cosf(angleRadians) * radius));
        const int16_t y = static_cast<int16_t>(roundf(centerY + sinf(angleRadians) * radius));
        const float tangentDegrees = angleDegrees + (reverse ? -90.0f : 90.0f);
        canvas.drawCenteredTextRotated(String(text.charAt(static_cast<unsigned int>(i))),
                                       x,
                                       y,
                                       tangentDegrees,
                                       style,
                                       textColor,
                                       bgColor);
    }
}

void drawArcText(RemoteCanvas& canvas,
                 const String& text,
                 int16_t centerX,
                 int16_t centerY,
                 int16_t radius,
                 float centerDegrees,
                 float spanDegrees,
                 bool reverse,
                 RemoteTextStyle style,
                 uint16_t textColor,
                 uint16_t bgColor,
                 uint16_t accentColor,
                 bool subtle) {
    if(text.length() == 0 || radius <= 0 || spanDegrees <= 0.0f) {
        return;
    }

    const int16_t charWidth = max<int16_t>(1, static_cast<int16_t>(6 * textScale(style)));
    const float charStepDegrees = max(4.0f,
                                      min(18.0f,
                                          static_cast<float>(charWidth + 2) * 57.2957795f /
                                              static_cast<float>(radius)));
    const float maxSpan = max(0.0f, spanDegrees - charStepDegrees);
    const float totalSpan = min(maxSpan, charStepDegrees * static_cast<float>(text.length() - 1));
    const float startDegrees = reverse ? (centerDegrees + (totalSpan * 0.5f)) : (centerDegrees - (totalSpan * 0.5f));
    const float stepDegrees = reverse ? -charStepDegrees : charStepDegrees;

    for(size_t i = 0; i < text.length(); ++i) {
        const float angleDegrees = startDegrees + (stepDegrees * static_cast<float>(i));
        const float angleRadians = angleDegrees * 0.01745329252f;
        const int16_t x = static_cast<int16_t>(roundf(centerX + cosf(angleRadians) * radius));
        const int16_t y = static_cast<int16_t>(roundf(centerY + sinf(angleRadians) * radius));
        const float tangentDegrees = angleDegrees + (reverse ? -90.0f : 90.0f);
        drawSoftCenteredTextRotated(canvas,
                                    String(text.charAt(static_cast<unsigned int>(i))),
                                    x,
                                    y,
                                    tangentDegrees,
                                    style,
                                    textColor,
                                    bgColor,
                                    accentColor,
                                    subtle);
    }
}

void drawRoundHomeArcLabels(RemoteCanvas& canvas, const Layout& layout, const HomePalette& palette) {
    const bool running = isRunningState() && homeWheelDisplayRpm() != 0;
    const String topLabel = fitTextToArc(roundHomeTopLabel(),
                                         static_cast<int16_t>(layout.homeDialOuterRadius + 82),
                                         120.0f,
                                         chooseArcTextStyle(roundHomeTopLabel(),
                                                            static_cast<int16_t>(layout.homeDialOuterRadius + 82),
                                                            120.0f,
                                                            true));
    if(topLabel.length() > 0) {
        const RemoteTextStyle topStyle =
            chooseArcTextStyle(topLabel, static_cast<int16_t>(layout.homeDialOuterRadius + 82), 120.0f, true);
        drawArcTextSimple(canvas,
                          topLabel,
                          layout.homeDialCx,
                          layout.homeDialCy,
                          static_cast<int16_t>(layout.homeDialOuterRadius + 82),
                          -90.0f,
                          120.0f,
                          false,
                          topStyle,
                          blendColor565(palette.muted, palette.accent, running ? 0.24f : 0.34f),
                          palette.background);
    }

    if(useRoundNavigation() && !running) {
        const String hintSource = "EDGE SWIPE FOR MENU";
        const int16_t hintRadius = static_cast<int16_t>(layout.homeDialOuterRadius + 62);
        const float hintSpan = 148.0f;
        const RemoteTextStyle hintStyle = chooseArcTextStyle(hintSource, hintRadius, hintSpan, false);
        const String hintLabel = fitTextToArc(hintSource, hintRadius, hintSpan, hintStyle);
        if(hintLabel.length() > 0) {
            drawArcTextSimple(canvas,
                              hintLabel,
                              layout.homeDialCx,
                              layout.homeDialCy,
                              hintRadius,
                              90.0f,
                              hintSpan,
                              true,
                              hintStyle,
                              blendColor565(palette.muted, palette.background, 0.38f),
                              palette.background);
        }
    }

    const String bottomSource = roundHomeBottomLabel();
    if(bottomSource.length() == 0) {
        return;
    }

    const int16_t bottomRadius = static_cast<int16_t>(layout.homeDialOuterRadius + 104);
    const bool bottomProminent = bottomSource.length() <= 18;
    const float bottomSpan = bottomSource.length() <= 14 ? 118.0f : 132.0f;
    const RemoteTextStyle bottomStyle = chooseArcTextStyle(bottomSource, bottomRadius, bottomSpan, bottomProminent);
    const String bottomLabel = fitTextToArc(bottomSource, bottomRadius, bottomSpan, bottomStyle);
    if(bottomLabel.length() == 0) {
        return;
    }

    drawArcTextSimple(canvas,
                      bottomLabel,
                      layout.homeDialCx,
                      layout.homeDialCy,
                      bottomRadius,
                      90.0f,
                      bottomSpan,
                      true,
                      bottomStyle,
                      blendColor565(palette.muted, palette.background, running ? 0.18f : 0.26f),
                      palette.background);
}

void drawButton(RemoteCanvas& canvas,
                const Rect& rect,
                const String& text,
                uint16_t fill,
                uint16_t textColor) {
    const RemoteTextStyle style = pillTextStyle();
    const String label = upperText(text);

    if(useRoundNavigation()) {
        drawNestCard(canvas, rect, fill, blendColor565(fill, kColorText, 0.24f));
        const uint16_t accentDot = blendColor565(textColor, fill, 0.18f);
        const int16_t dotRadius = rect.h >= 30 ? 4 : 3;
        const bool narrow = rect.w <= 60;
        const RemoteTextStyle roundStyle = narrow ? RemoteTextStyle::Tiny : style;
        const int16_t padding = narrow ? 6 : 16;
        canvas.fillCircle(static_cast<int16_t>(rect.x + rect.w / 2),
                          static_cast<int16_t>(rect.y + rect.h - 9),
                          dotRadius,
                          accentDot);
        const int split = narrow ? label.indexOf(' ') : -1;
        if(split > 0 && split < (label.length() - 1)) {
            const String top = fitTextToWidth(label.substring(0, split), rect.w, roundStyle, 4);
            const String bottom = fitTextToWidth(label.substring(split + 1), rect.w, roundStyle, 4);
            canvas.drawCenteredText(top,
                                    rect.x + rect.w / 2,
                                    static_cast<int16_t>(rect.y + rect.h / 2 - 7),
                                    roundStyle,
                                    textColor,
                                    fill);
            canvas.drawCenteredText(bottom,
                                    rect.x + rect.w / 2,
                                    static_cast<int16_t>(rect.y + rect.h / 2 + 6),
                                    roundStyle,
                                    textColor,
                                    fill);
        } else {
            canvas.drawCenteredText(fitTextToWidth(label, rect.w, roundStyle, padding),
                                    rect.x + rect.w / 2,
                                    static_cast<int16_t>(rect.y + rect.h / 2 - (narrow ? 1 : 2)),
                                    roundStyle,
                                    textColor,
                                    fill);
        }
        return;
    }

    const int16_t radius = rect.h / 2;
    canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, fill);
    canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, kColorPanelHi);
    canvas.drawCenteredText(fitTextToWidth(label, rect.w, style),
                            rect.x + rect.w / 2,
                            rect.y + rect.h / 2,
                            style,
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

float normalizedRangeValue(int32_t value, int32_t minValue, int32_t maxValue) {
    if(maxValue <= minValue) {
        return 0.0f;
    }
    return clampUnit(static_cast<float>(value - minValue) / static_cast<float>(maxValue - minValue));
}

float normalizedHomeLevel() {
    if(isGbwState()) {
        return normalizedRangeValue(app.weightMg, default_min_weight, default_max_weight);
    }
    const int32_t minRpm = app.menuValues[1][SETMIN] > 0 ? app.menuValues[1][SETMIN] : default_minRPM;
    const int32_t maxRpm = app.menuValues[1][SETMAX] > minRpm ? app.menuValues[1][SETMAX] : default_maxRPM;
    return normalizedRangeValue(app.manualRpm, minRpm, maxRpm);
}

float normalizedDisplayedHomeLevel() {
    if(isGbwState()) {
        const int32_t shownWeight =
            (isRunningState() && app.hasStatus && app.status.currentWeightMg > 0) ? app.status.currentWeightMg : app.weightMg;
        return normalizedRangeValue(shownWeight, default_min_weight, default_max_weight);
    }

    const int32_t minRpm = app.menuValues[1][SETMIN] > 0 ? app.menuValues[1][SETMIN] : default_minRPM;
    const int32_t maxRpm = app.menuValues[1][SETMAX] > minRpm ? app.menuValues[1][SETMAX] : default_maxRPM;
    const int32_t shownRpm = isRunningState() ? homeWheelDisplayRpm() : app.manualRpm;
    return normalizedRangeValue(rpmMagnitude(shownRpm), minRpm, maxRpm);
}

float normalizedItemLevel(const SettingItem& item, int32_t value) {
    if(usesChoiceEditor(item)) {
        return value != 0 ? 1.0f : 0.0f;
    }
    return normalizedRangeValue(value, item.minValue, item.maxValue);
}

uint16_t ringColorForLevel(float level) {
    if(level < 0.5f) {
        return blendColor565(kColorGood, kColorAccent, level * 2.0f);
    }
    if(level < 0.82f) {
        return blendColor565(kColorAccent, kColorWarn, (level - 0.5f) / 0.32f);
    }
    return blendColor565(kColorWarn, kColorDanger, (level - 0.82f) / 0.18f);
}

size_t wrapMenuItemIndex(size_t selected, int delta, size_t count) {
    if(count == 0) {
        return 0;
    }

    int idx = static_cast<int>(selected) + delta;
    while(idx < 0) {
        idx += static_cast<int>(count);
    }
    while(idx >= static_cast<int>(count)) {
        idx -= static_cast<int>(count);
    }
    return static_cast<size_t>(idx);
}

bool roundMenuItemShowsValuePreview(const SettingItem& item) {
    return item.kind == ItemKind::Value ||
           item.kind == ItemKind::Toggle ||
           isEspNowPeerSlotItem(item) ||
           isEspNowPeerScanItem(item) ||
           isEspNowPeerClearItem(item) ||
           isRadioModeItem(item) ||
           (item.action != nullptr && strcmp(item.action, "screen") == 0);
}

uint16_t roundMenuItemAccentColor(const SettingItem& item) {
    if(item.kind == ItemKind::Danger) {
        return kColorDanger;
    }
    if(item.kind == ItemKind::Action) {
        return kColorAccent2;
    }
    if(item.kind == ItemKind::Toggle) {
        return blendColor565(kColorAccent, kColorPanelHi, 0.24f);
    }
    if(item.kind == ItemKind::Value) {
        return ringColorForLevel(normalizedItemLevel(item, menuValue(item)));
    }
    return blendColor565(kColorAccent, kColorPanelHi, 0.18f);
}

void drawLevelRing(RemoteCanvas& canvas,
                   int16_t centerX,
                   int16_t centerY,
                   int16_t innerRadius,
                   int16_t outerRadius,
                   float level,
                   uint16_t trackColor,
                   uint16_t glowColor) {
    const float startAngle = static_cast<float>(PI) * 0.68f;
    const float sweepAngle = static_cast<float>(PI) * 1.62f;
    const int segments = isLargeCanvas() ? 120 : 80;
    const float clampedLevel = clampUnit(level);
    const uint16_t activeStart = blendColor565(glowColor, kColorText, 0.18f);
    const uint16_t activeEnd = ringColorForLevel(clampedLevel);

    for(int i = 0; i < segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments - 1);
        const float angle = startAngle + (sweepAngle * t);
        uint16_t color = trackColor;
        if(t <= clampedLevel) {
            const float colorT = clampedLevel <= 0.0f ? 0.0f : (t / clampedLevel);
            color = blendColor565(activeStart, activeEnd, colorT);
        }
        const int16_t x0 = static_cast<int16_t>(centerX + cosf(angle) * innerRadius);
        const int16_t y0 = static_cast<int16_t>(centerY + sinf(angle) * innerRadius);
        const int16_t x1 = static_cast<int16_t>(centerX + cosf(angle) * outerRadius);
        const int16_t y1 = static_cast<int16_t>(centerY + sinf(angle) * outerRadius);
        canvas.drawLine(x0, y0, x1, y1, color);
    }

    if(clampedLevel > 0.0f) {
        const float tipAngle = startAngle + (sweepAngle * clampedLevel);
        const int16_t tipRadius = static_cast<int16_t>((innerRadius + outerRadius) / 2);
        const int16_t tipX = static_cast<int16_t>(centerX + cosf(tipAngle) * tipRadius);
        const int16_t tipY = static_cast<int16_t>(centerY + sinf(tipAngle) * tipRadius);
        canvas.fillCircle(tipX,
                          tipY,
                          isLargeCanvas() ? 6 : 4,
                          blendColor565(activeEnd, kColorText, 0.22f));
    }
}

void drawBurrTooth(RemoteCanvas& canvas,
                   int16_t centerX,
                   int16_t centerY,
                   int16_t baseRadius,
                   int16_t tipRadius,
                   float angle,
                   int16_t width,
                   uint16_t color) {
    const float nx = cosf(angle);
    const float ny = sinf(angle);
    const float px = -ny;
    const float py = nx;
    const int16_t x0 = static_cast<int16_t>(roundf(centerX + nx * baseRadius));
    const int16_t y0 = static_cast<int16_t>(roundf(centerY + ny * baseRadius));
    const int16_t x1 = static_cast<int16_t>(roundf(centerX + nx * tipRadius));
    const int16_t y1 = static_cast<int16_t>(roundf(centerY + ny * tipRadius));
    for(int16_t offset = -width; offset <= width; ++offset) {
        const int16_t ox = static_cast<int16_t>(roundf(px * static_cast<float>(offset)));
        const int16_t oy = static_cast<int16_t>(roundf(py * static_cast<float>(offset)));
        canvas.drawLine(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
    }
}

void drawBurrRing(RemoteCanvas& canvas,
                  int16_t centerX,
                  int16_t centerY,
                  int16_t innerRadius,
                  int16_t outerRadius,
                  int16_t toothBaseRadius,
                  int16_t toothTipRadius,
                  uint8_t toothCount,
                  float phase,
                  int16_t toothWidth,
                  uint16_t ringColor,
                  uint16_t toothColor) {
    canvas.drawCircle(centerX, centerY, innerRadius, ringColor);
    canvas.drawCircle(centerX, centerY, outerRadius, ringColor);
    for(uint8_t i = 0; i < toothCount; ++i) {
        const float angle = phase + (static_cast<float>(i) / static_cast<float>(toothCount)) * static_cast<float>(2.0 * PI);
        drawBurrTooth(canvas, centerX, centerY, toothBaseRadius, toothTipRadius, angle, toothWidth, toothColor);
    }
}

void drawHomeWheel(RemoteCanvas& canvas, const Layout& layout) {
    const HomePalette palette = buildHomePalette();
    const float phase = app.homeWheelAngle;
    const float speed = homeWheelTurnsPerSecond();
    const int8_t direction = homeWheelSpinDirection();
    const float innerPhase = phase;
    const float outerPhase = phase * -0.45f;
    const uint16_t staticRing = blendColor565(palette.panelHi, palette.background, 0.08f);
    const uint16_t movingRing = blendColor565(palette.accent, palette.panelHi, 0.30f);
    const uint16_t movingTeeth = blendColor565(palette.accent2, kColorText, 0.20f);
    const uint16_t staticTeeth = blendColor565(palette.muted, palette.background, 0.06f);

    drawBurrRing(canvas,
                 layout.homeDialCx,
                 layout.homeDialCy,
                 static_cast<int16_t>(layout.homeDialInnerRadius + 18),
                 static_cast<int16_t>(layout.homeDialInnerRadius + 44),
                 static_cast<int16_t>(layout.homeDialInnerRadius + 28),
                 static_cast<int16_t>(layout.homeDialInnerRadius + 44),
                 18,
                 outerPhase,
                 isLargeCanvas() ? 2 : 1,
                 staticRing,
                 staticTeeth);

    if(direction != 0 && speed > 0.0f) {
        drawBurrRing(canvas,
                     layout.homeDialCx,
                     layout.homeDialCy,
                     static_cast<int16_t>(layout.homeDialOuterRadius - 54),
                     static_cast<int16_t>(layout.homeDialOuterRadius - 20),
                     static_cast<int16_t>(layout.homeDialOuterRadius - 42),
                     static_cast<int16_t>(layout.homeDialOuterRadius - 20),
                     14,
                     innerPhase * static_cast<float>(direction),
                     isLargeCanvas() ? 3 : 2,
                     movingRing,
                     movingTeeth);
    }

    canvas.fillCircle(layout.homeDialCx,
                      layout.homeDialCy,
                      static_cast<int16_t>(layout.homeDialInnerRadius + 8),
                      blendColor565(palette.panel, palette.background, 0.16f));
    canvas.drawCircle(layout.homeDialCx,
                      layout.homeDialCy,
                      static_cast<int16_t>(layout.homeDialInnerRadius + 8),
                      blendColor565(palette.panelHi, palette.background, 0.10f));
}

void drawTabs(RemoteCanvas& canvas, const Layout& layout) {
    for(uint8_t i = 0; i < 5; ++i) {
        const bool active = static_cast<uint8_t>(app.screen) == i;
        const uint16_t fill = active ? kColorAccent : kColorPanel;
        const uint16_t textColor = active ? kColorBlack : kColorMuted;
        drawButton(canvas, layout.tabs[i], kTabLabels[i], fill, textColor);
    }
}

void drawChevron(RemoteCanvas& canvas,
                 int16_t centerX,
                 int16_t centerY,
                 int16_t size,
                 bool left,
                 uint16_t color) {
    const int16_t half = static_cast<int16_t>(size / 2);
    const int16_t tipX = static_cast<int16_t>(centerX + (left ? -half : half));
    const int16_t tailX = static_cast<int16_t>(centerX + (left ? half : -half));
    const int16_t y0 = static_cast<int16_t>(centerY - half);
    const int16_t y1 = static_cast<int16_t>(centerY + half);
    for(int8_t offset = -1; offset <= 1; ++offset) {
        canvas.drawLine(tipX + offset, centerY, tailX + offset, y0, color);
        canvas.drawLine(tipX + offset, centerY, tailX + offset, y1, color);
    }
}

void drawRoundNav(RemoteCanvas& canvas, const Layout& layout) {
    if(useWaveshareRoundUi()) {
        if(!isRoundTopLevelTabScreen(app.screen)) {
            return;
        }
        const uint16_t hintColor = blendColor565(kColorMuted, kColorBg, 0.10f);
        drawChevron(canvas,
                    static_cast<int16_t>(layout.navPrev.x + 16),
                    static_cast<int16_t>(layout.navPrev.y + layout.navPrev.h / 2),
                    12,
                    true,
                    hintColor);
        drawChevron(canvas,
                    static_cast<int16_t>(layout.navNext.x + layout.navNext.w - 16),
                    static_cast<int16_t>(layout.navNext.y + layout.navNext.h / 2),
                    12,
                    false,
                    hintColor);
        return;
    }

    const Screen prevScreen = roundNavMenuTarget(app.screen, -1);
    const Screen nextScreen = roundNavMenuTarget(app.screen, 1);
    const uint16_t shellColor = blendColor565(kColorPanel, kColorBg, 0.34f);
    const uint16_t borderColor = blendColor565(shellColor, kColorBg, 0.08f);

    drawNestCard(canvas, layout.navPrev, shellColor, borderColor, true);
    drawChevron(canvas,
                static_cast<int16_t>(layout.navPrev.x + layout.navPrev.w / 2),
                static_cast<int16_t>(layout.navPrev.y + layout.navPrev.h / 2 - 16),
                22,
                true,
                blendColor565(kColorText, kColorBg, 0.14f));
    drawSoftCenteredText(canvas,
                         fitTextToWidth(screenTitle(prevScreen), layout.navPrev.w, pillTextStyle(), 8),
                         static_cast<int16_t>(layout.navPrev.x + layout.navPrev.w / 2),
                         static_cast<int16_t>(layout.navPrev.y + layout.navPrev.h / 2 + 28),
                         pillTextStyle(),
                         blendColor565(kColorMuted, kColorBg, 0.10f),
                         shellColor,
                         shellColor,
                         true);

    drawNestCard(canvas, layout.navNext, shellColor, borderColor, true);
    drawChevron(canvas,
                static_cast<int16_t>(layout.navNext.x + layout.navNext.w / 2),
                static_cast<int16_t>(layout.navNext.y + layout.navNext.h / 2 - 16),
                22,
                false,
                blendColor565(kColorText, kColorBg, 0.14f));
    drawSoftCenteredText(canvas,
                         fitTextToWidth(screenTitle(nextScreen), layout.navNext.w, pillTextStyle(), 8),
                         static_cast<int16_t>(layout.navNext.x + layout.navNext.w / 2),
                         static_cast<int16_t>(layout.navNext.y + layout.navNext.h / 2 + 28),
                         pillTextStyle(),
                         blendColor565(kColorMuted, kColorBg, 0.10f),
                         shellColor,
                         shellColor,
                         true);
}

void buildToggleChoiceRects(const Layout& layout, Rect* offRect, Rect* onRect) {
    if(offRect == nullptr || onRect == nullptr) {
        return;
    }

    const int16_t gap = isLargeCanvas() ? 14 : 10;
    const int16_t rowW = clampI16(static_cast<int16_t>(layout.width - (layout.pad * 6)), 210, 300);
    const int16_t rowH = clampI16(static_cast<int16_t>(layout.buttonH + 14), 56, 74);
    const int16_t x = static_cast<int16_t>(layout.editorCx - (rowW / 2));
    const int16_t startY = static_cast<int16_t>(layout.editorCy - rowH - (gap / 2));
    *offRect = {x, startY, rowW, rowH};
    *onRect = {x, static_cast<int16_t>(startY + rowH + gap), rowW, rowH};
}

void drawFooter(RemoteCanvas& canvas, const Layout& layout, const String& text) {
    if(text.length() == 0) {
        return;
    }
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
            return "CONNECTING";
        }
        return "OFFLINE";
    }

    if(app.currentTransport == LinkTransport::EspNow) {
        return "ESP-NOW";
    }

    return "OFFLINE";
}

void drawFooter(RemoteCanvas& canvas, const Layout& layout) {
    drawFooter(canvas, layout, transportStatusText());
}

Rect homeStatusRect(const Layout& layout) {
    if(isLargeCanvas()) {
        const int16_t width = clampI16(static_cast<int16_t>(layout.width - (layout.pad * 6) - 96), 210, 280);
        const int16_t height = clampI16(static_cast<int16_t>(layout.topPillH - 2), 24, 30);
        const int16_t x = static_cast<int16_t>((layout.width - width) / 2);
        const int16_t y = static_cast<int16_t>(layout.height - layout.pad - height - 14);
        return {x, y, width, height};
    }

    const int16_t width = clampI16(static_cast<int16_t>(layout.width - (layout.pad * 4)), 220, 320);
    const int16_t x = static_cast<int16_t>((layout.width - width) / 2);
    const int16_t y = static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + 6);
    const int16_t maxBottom = static_cast<int16_t>(layout.height - layout.pad - 6);
    const int16_t height = clampI16(static_cast<int16_t>(layout.topPillH - 4), 20, 28);
    if(y + height > maxBottom) {
        return {x, static_cast<int16_t>(maxBottom - height), width, height};
    }
    return {x, y, width, height};
}

Rect shiftRectY(Rect rect, float offset) {
    rect.y = static_cast<int16_t>(rect.y + lroundf(offset));
    return rect;
}

Rect expandRect(Rect rect, int16_t expandX, int16_t expandY) {
    rect.x = static_cast<int16_t>(rect.x - expandX);
    rect.y = static_cast<int16_t>(rect.y - expandY);
    rect.w = static_cast<int16_t>(rect.w + (expandX * 2));
    rect.h = static_cast<int16_t>(rect.h + (expandY * 2));
    return rect;
}

void buildRoundMenuTouchRects(const Layout& layout, Rect* prevRect, Rect* heroRect, Rect* nextRect) {
    const int16_t sideGap = 18;
    const Rect content = {
        static_cast<int16_t>(layout.navPrev.x + layout.navPrev.w + sideGap),
        0,
        static_cast<int16_t>(layout.navNext.x - sideGap - (layout.navPrev.x + layout.navPrev.w + sideGap)),
        layout.height
    };
    const Rect hero = shiftRectY(layout.menuRows[1], app.menuRollOffset);
    if(prevRect != nullptr) {
        const int16_t topBottomGap = 8;
        const int16_t height = max<int16_t>(28, static_cast<int16_t>(hero.y - content.y - topBottomGap));
        *prevRect = {
            content.x,
            content.y,
            content.w,
            height
        };
    }
    if(heroRect != nullptr) {
        const int16_t heroInsetX = 12;
        const int16_t heroInsetY = 18;
        *heroRect = {
            static_cast<int16_t>(hero.x + heroInsetX),
            static_cast<int16_t>(hero.y + heroInsetY),
            max<int16_t>(28, static_cast<int16_t>(hero.w - heroInsetX * 2)),
            max<int16_t>(40, static_cast<int16_t>(hero.h - heroInsetY * 2))
        };
    }
    if(nextRect != nullptr) {
        const int16_t topBottomGap = 8;
        const int16_t y = static_cast<int16_t>(hero.y + hero.h + topBottomGap);
        const int16_t height = max<int16_t>(28, static_cast<int16_t>(content.y + content.h - y));
        *nextRect = {
            content.x,
            y,
            content.w,
            height
        };
    }
}

Rect roundMenuContentRect(const Layout& layout) {
    const int16_t sideGap = 18;
    const int16_t left = static_cast<int16_t>(layout.navPrev.x + layout.navPrev.w + sideGap);
    const int16_t right = static_cast<int16_t>(layout.navNext.x - sideGap);
    return {left, 0, static_cast<int16_t>(right - left), layout.height};
}

Rect homeButtonTouchRect(const Rect& rect, bool leftButton) {
    Rect hit = rect;
    const int16_t verticalExpand = useRoundNavigation() ? 18 : 6;
    hit.y = static_cast<int16_t>(hit.y - verticalExpand);
    hit.h = static_cast<int16_t>(hit.h + verticalExpand * 2);
    if(leftButton) {
        hit.x = static_cast<int16_t>(hit.x + 10);
        hit.w = static_cast<int16_t>(hit.w + 28);
    } else {
        hit.x = static_cast<int16_t>(hit.x - 40);
        hit.w = static_cast<int16_t>(hit.w + 44);
    }
    return hit;
}

Rect homeModeTouchRect(const Layout& layout) {
    if(!useRoundNavigation()) {
        return homeButtonTouchRect(layout.homeModeButton, true);
    }

    const int16_t railBottom = static_cast<int16_t>(layout.homeDialCy + layout.homeDialOuterRadius + 58);
    const int16_t railTop = static_cast<int16_t>(layout.homeDialCy - layout.homeDialOuterRadius - 58);
    const int16_t railRight = static_cast<int16_t>(layout.homeDialCx - layout.homeDialOuterRadius - 24);
    return {
        0,
        static_cast<int16_t>(max<int16_t>(0, railTop)),
        static_cast<int16_t>(max<int16_t>(40, railRight)),
        static_cast<int16_t>(min<int16_t>(layout.height, railBottom) - max<int16_t>(0, railTop))
    };
}

Rect homeActionTouchRect(const Layout& layout) {
    if(!useRoundNavigation()) {
        return homeButtonTouchRect(layout.homeActionButton, false);
    }

    const int16_t railLeft = static_cast<int16_t>(layout.homeDialCx + layout.homeDialInnerRadius + 8);
    const int16_t railTop = static_cast<int16_t>(layout.homeDialCy - layout.homeDialOuterRadius - 58);
    const int16_t railBottom = static_cast<int16_t>(layout.homeDialCy + layout.homeDialOuterRadius + 58);
    const int16_t x = max<int16_t>(0, railLeft);
    return {
        x,
        static_cast<int16_t>(max<int16_t>(0, railTop)),
        static_cast<int16_t>(layout.width - x),
        static_cast<int16_t>(min<int16_t>(layout.height, railBottom) - max<int16_t>(0, railTop))
    };
}

Rect homeMenuTopEdgeRect(const Layout& layout) {
    if(!useRoundNavigation()) {
        return {};
    }

    const int16_t edgeH = clampI16(static_cast<int16_t>(layout.pad + 34), 42, 62);
    return {0, 0, layout.width, edgeH};
}

Rect homeMenuBottomEdgeRect(const Layout& layout) {
    if(!useRoundNavigation()) {
        return {};
    }

    const int16_t edgeH = clampI16(static_cast<int16_t>(layout.pad + 38), 46, 68);
    return {0, static_cast<int16_t>(layout.height - edgeH), layout.width, edgeH};
}

int16_t roundMenuScrollThreshold(const Layout& layout) {
    const int16_t stepDistance = abs(static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y));
    if(stepDistance > 0) {
        return clampI16(static_cast<int16_t>((stepDistance * 3) / 4), 24, 48);
    }
    return clampI16(static_cast<int16_t>(layout.rowH / 3), 24, 48);
}

void playUiStepTick(uint8_t count = 1) {
    if(app.localUiSounds == 0) {
        return;
    }

    for(uint8_t i = 0; i < count; ++i) {
        remote_audio_play(RemoteAudioTone::Click);
    }
}

void syncTouchScrollPreviewAudio(int16_t previewSteps) {
    while(touchTracker.previewAudioSteps < previewSteps) {
        ++touchTracker.previewAudioSteps;
        playUiStepTick();
    }
    while(touchTracker.previewAudioSteps > previewSteps) {
        --touchTracker.previewAudioSteps;
        playUiStepTick();
    }
}

void drawWaveshareRoundMenuList(RemoteCanvas& canvas, const Layout& layout, const char* title) {
    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items == nullptr || count == 0) {
        drawPill(canvas, layout.modeBadge, title, kColorPanel, kColorText);
        drawFooter(canvas, layout, "No items");
        drawRoundNav(canvas, layout);
        return;
    }

    const uint8_t selected = selectionFor(app.screen);
    const Rect content = roundMenuContentRect(layout);
    const int16_t centerX = static_cast<int16_t>(content.x + content.w / 2);
    const int16_t centerY = static_cast<int16_t>(layout.menuRows[1].y + layout.menuRows[1].h / 2);
    const int16_t stepDistance = max<int16_t>(1, abs(static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y)));
    const float offsetSteps = app.menuRollOffset / static_cast<float>(stepDistance);
    const int16_t farInsetMax = clampI16(static_cast<int16_t>(content.w / 9), 12, 22);
    size_t drawnIndices[5] = {};
    uint8_t drawnCount = 0;

    drawPill(canvas, layout.modeBadge, title, kColorPanel, kColorText);

    if(count == 1) {
        const SettingItem& item = items[0];
        const uint16_t accent = roundMenuItemAccentColor(item);
        const int16_t rowW = max<int16_t>(200, static_cast<int16_t>(content.w - 16));
        const int16_t rowH = clampI16(static_cast<int16_t>(layout.rowH + 8), 52, 72);
        const Rect rowRect = {
            static_cast<int16_t>(centerX - (rowW / 2)),
            static_cast<int16_t>(centerY - (rowH / 2)),
            rowW,
            rowH
        };
        const uint16_t baseFace = blendColor565(kColorPanel, accent, 0.28f);
        const uint16_t face = blendColor565(kColorBg, baseFace, 0.84f);
        const uint16_t border = blendColor565(accent, kColorText, 0.20f);
        const uint16_t textColor = blendColor565(kColorText, accent, 0.08f);
        const int16_t radius = clampI16(static_cast<int16_t>(rowRect.h / 2), 18, 28);
        const String label = upperText(displayLabelForItem(item));
        const RemoteTextStyle labelStyle = chooseMenuHeroTextStyle(label, static_cast<int16_t>(rowRect.w - 20));

        canvas.fillRoundRect(rowRect.x, rowRect.y, rowRect.w, rowRect.h, radius, face);
        canvas.drawRoundRect(rowRect.x, rowRect.y, rowRect.w, rowRect.h, radius, border);
        if(rowRect.w > 8 && rowRect.h > 8) {
            const int16_t innerRadius = clampI16(static_cast<int16_t>(radius - 2), 12, 24);
            canvas.drawRoundRect(rowRect.x + 2,
                                 rowRect.y + 2,
                                 rowRect.w - 4,
                                 rowRect.h - 4,
                                 innerRadius,
                                 blendColor565(border, kColorText, 0.18f));
        }
        drawSoftCenteredText(canvas,
                             fitTextToWidthNoTilde(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4),
                             centerX,
                             static_cast<int16_t>(rowRect.y + rowRect.h / 2),
                             labelStyle,
                             textColor,
                             face,
                             accent,
                             false);

        const String footerText = (app.toastUntil > millis()) ? upperText(app.toast) : String("SWIPE TO BROWSE");
        drawFooter(canvas, layout, footerText);
        drawRoundNav(canvas, layout);
        return;
    }

    if(count == 2) {
        const uint8_t selected = selectionFor(app.screen);
        const uint8_t other = static_cast<uint8_t>(wrapMenuItemIndex(selected, 1, count));
        const Rect content = roundMenuContentRect(layout);
        const int16_t centerX = static_cast<int16_t>(content.x + content.w / 2);
        const int16_t centerY = static_cast<int16_t>(layout.menuRows[1].y + layout.menuRows[1].h / 2);
        const int16_t stepDistance = max<int16_t>(1, abs(static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y)));
        const float offsetSteps = app.menuRollOffset / static_cast<float>(stepDistance);
        const int16_t farInsetMax = clampI16(static_cast<int16_t>(content.w / 9), 12, 22);
        const int relOrder[] = {-1, 1, 0};

        drawPill(canvas, layout.modeBadge, title, kColorPanel, kColorText);

        for(size_t i = 0; i < (sizeof(relOrder) / sizeof(relOrder[0])); ++i) {
            const int rel = relOrder[i];
            const float visualOffset = static_cast<float>(rel) + offsetSteps;
            const float distance = fabsf(visualOffset);
            if(distance > 1.35f) {
                continue;
            }

            float prominence = 1.0f - (distance / 1.35f);
            if(prominence < 0.0f) {
                prominence = 0.0f;
            }
            prominence *= prominence;

            const bool focused = rel == 0;
            const SettingItem& item = items[focused ? selected : other];
            const bool dangerArmed = focused && item.kind == ItemKind::Danger && dangerIsArmed(app.screen, selected);
            const uint16_t accent = dangerArmed ? kColorDanger : roundMenuItemAccentColor(item);
            const int16_t inset =
                focused ? 8 : clampI16(static_cast<int16_t>(8.0f + distance * static_cast<float>(farInsetMax)), 8, farInsetMax);
            const int16_t rowW = focused ? max<int16_t>(200, static_cast<int16_t>(content.w - 16))
                                         : max<int16_t>(180, static_cast<int16_t>(content.w - inset * 2));
            const int16_t rowH = focused ? clampI16(static_cast<int16_t>(layout.rowH + 8), 52, 72)
                                         : clampI16(static_cast<int16_t>(46.0f + prominence * 18.0f), 44, 62);
            const int16_t rowX = static_cast<int16_t>(centerX - (rowW / 2));
            const int16_t rowY =
                static_cast<int16_t>(centerY + lroundf(visualOffset * static_cast<float>(stepDistance)) - (rowH / 2));
            const Rect rowRect = {rowX, rowY, rowW, rowH};
            const uint16_t baseFace = blendColor565(kColorPanel, accent, focused ? 0.28f : (0.10f + prominence * 0.20f));
            const uint16_t face = blendColor565(kColorBg, baseFace, focused ? 0.84f : (0.18f + prominence * 0.70f));
            const uint16_t border = focused ? blendColor565(accent, kColorText, 0.20f)
                                            : blendColor565(face, accent, 0.10f + prominence * 0.12f);
            const uint16_t textBase = blendColor565(kColorMuted, kColorBg, 0.16f);
            const uint16_t textColor = focused ? blendColor565(kColorText, accent, 0.08f)
                                               : blendColor565(textBase, kColorText, 0.18f + prominence * 0.72f);
            const int16_t radius = clampI16(static_cast<int16_t>(rowRect.h / 2), 16, 28);
            const String label = upperText(displayLabelForItem(item));
            const RemoteTextStyle labelStyle =
                focused ? chooseMenuHeroTextStyle(label, static_cast<int16_t>(rowRect.w - 20))
                        : (distance < 1.05f ? chooseLabelTextStyle(label, static_cast<int16_t>(rowRect.w - 18))
                                            : RemoteTextStyle::Small);

            canvas.fillRoundRect(rowRect.x, rowRect.y, rowRect.w, rowRect.h, radius, face);
            canvas.drawRoundRect(rowRect.x, rowRect.y, rowRect.w, rowRect.h, radius, border);
            if(focused && rowRect.w > 8 && rowRect.h > 8) {
                const int16_t innerRadius = clampI16(static_cast<int16_t>(radius - 2), 12, 24);
                canvas.drawRoundRect(rowRect.x + 2,
                                     rowRect.y + 2,
                                     rowRect.w - 4,
                                     rowRect.h - 4,
                                     innerRadius,
                                     blendColor565(border, kColorText, 0.18f));
            }

            drawSoftCenteredText(canvas,
                                 fitTextToWidthNoTilde(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4),
                                 centerX,
                                 static_cast<int16_t>(rowRect.y + rowRect.h / 2),
                                 labelStyle,
                                 textColor,
                                 face,
                                 accent,
                                 !focused);
        }

        const String footerText = (app.toastUntil > millis()) ? upperText(app.toast) : String("SWIPE TO BROWSE");
        drawFooter(canvas, layout, footerText);
        drawRoundNav(canvas, layout);
        return;
    }
    for(int rel = -2; rel <= 2; ++rel) {
        const float visualOffset = static_cast<float>(rel) + offsetSteps;
        const float distance = fabsf(visualOffset);
        if(distance > 2.35f) {
            continue;
        }

        float prominence = 1.0f - (distance / 2.35f);
        if(prominence < 0.0f) {
            prominence = 0.0f;
        }
        prominence *= prominence;

        const size_t idx = wrapMenuItemIndex(selected, rel, count);
        bool duplicateIndex = false;
        for(uint8_t i = 0; i < drawnCount; ++i) {
            if(drawnIndices[i] == idx) {
                duplicateIndex = true;
                break;
            }
        }
        if(duplicateIndex) {
            continue;
        }
        drawnIndices[drawnCount++] = idx;
        const SettingItem& item = items[idx];
        const bool focused = distance < 0.55f;
        const bool dangerArmed = focused && item.kind == ItemKind::Danger && dangerIsArmed(app.screen, selected);
        const uint16_t accent = dangerArmed ? kColorDanger : roundMenuItemAccentColor(item);
        const int16_t inset = clampI16(static_cast<int16_t>(8.0f + distance * static_cast<float>(farInsetMax)), 8, farInsetMax);
        const int16_t rowW = max<int16_t>(180, static_cast<int16_t>(content.w - inset * 2));
        const int16_t rowH = clampI16(static_cast<int16_t>(46.0f + prominence * 24.0f), 44, 70);
        const int16_t rowX = static_cast<int16_t>(centerX - (rowW / 2));
        const int16_t rowY = static_cast<int16_t>(centerY + lroundf(visualOffset * static_cast<float>(stepDistance)) - (rowH / 2));
        if(rowY + rowH < layout.modeBadge.y + layout.modeBadge.h || rowY > layout.footer.y) {
            continue;
        }

        const Rect rowRect = {rowX, rowY, rowW, rowH};
        const uint16_t baseFace = blendColor565(kColorPanel, accent, 0.10f + prominence * 0.22f);
        const uint16_t face = blendColor565(kColorBg, baseFace, 0.18f + prominence * 0.72f);
        const uint16_t border = focused ? blendColor565(accent, kColorText, 0.20f)
                                        : blendColor565(face, accent, 0.08f + prominence * 0.16f);
        const uint16_t textBase = blendColor565(kColorMuted, kColorBg, 0.16f);
        const uint16_t textColor = blendColor565(textBase, kColorText, 0.18f + prominence * 0.82f);
        const int16_t radius = clampI16(static_cast<int16_t>(rowRect.h / 2), 16, 26);

        canvas.fillRoundRect(rowRect.x, rowRect.y, rowRect.w, rowRect.h, radius, face);
        canvas.drawRoundRect(rowRect.x, rowRect.y, rowRect.w, rowRect.h, radius, border);
        if(focused && rowRect.w > 8 && rowRect.h > 8) {
            const int16_t innerRadius = clampI16(static_cast<int16_t>(radius - 2), 12, 22);
            canvas.drawRoundRect(rowRect.x + 2,
                                 rowRect.y + 2,
                                 rowRect.w - 4,
                                 rowRect.h - 4,
                                 innerRadius,
                                 blendColor565(border, kColorText, 0.18f));
        }

        const String label = upperText(displayLabelForItem(item));
        const RemoteTextStyle labelStyle =
            focused ? chooseMenuHeroTextStyle(label, static_cast<int16_t>(rowRect.w - 20))
                    : (distance < 1.15f ? chooseLabelTextStyle(label, static_cast<int16_t>(rowRect.w - 18))
                                        : RemoteTextStyle::Small);
        drawSoftCenteredText(canvas,
                             fitTextToWidthNoTilde(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4),
                             centerX,
                             static_cast<int16_t>(rowRect.y + rowRect.h / 2),
                             labelStyle,
                             textColor,
                             face,
                             accent,
                             distance > 1.15f);
    }

    const String footerText = (app.toastUntil > millis()) ? upperText(app.toast) : String("SWIPE TO BROWSE");
    drawFooter(canvas, layout, footerText);
    drawRoundNav(canvas, layout);
}

void drawHome(RemoteCanvas& canvas, const Layout& layout) {
    const HomePalette palette = buildHomePalette();
    canvas.fillScreen(palette.background);

    if(useRoundNavigation()) {
        const bool interactionActive = roundUiInteractionActive();
        const float level = normalizedDisplayedHomeLevel();
        const uint32_t now = millis();
        const bool showTargetValue = !isRunningState() || app.targetDirty || (now - app.lastHomeAdjustAt) < 1200;
        const bool showLiveValue = !showTargetValue && app.hasStatus;
        const int32_t liveRpm = homeWheelDisplayRpm();
        const bool reverseLiveRpm = !isGbwState() && showLiveValue && liveRpm < 0;
        const String targetValue = isGbwState() ? weightValueText(app.weightMg) : String(app.manualRpm);
        const String liveValue = isGbwState() ? weightValueText(app.status.currentWeightMg) : String(liveRpm);
        const String primaryValue = showLiveValue ? liveValue : targetValue;
        const String unitText = isGbwState() ? "GRAMS" : "RPM";
        const int16_t valueWidth = static_cast<int16_t>((layout.homeDialInnerRadius * 2) + 72);
        const int16_t unitWidth = static_cast<int16_t>((layout.homeDialInnerRadius * 2) + 28);
        const RemoteTextStyle valueStyle = chooseValueTextStyle(primaryValue, valueWidth);
        const RemoteTextStyle unitStyle = chooseLabelTextStyle(unitText, unitWidth);
        const uint16_t ringAccent = reverseLiveRpm ? blendColor565(kColorDanger, kColorWarn, 0.28f) : palette.accent;
        const uint16_t valueColor = isGbwState()
                                        ? blendColor565(kColorText, palette.accent, 0.24f)
                                        : (reverseLiveRpm ? blendColor565(kColorText, kColorDanger, 0.78f)
                                                          : blendColor565(kColorText, ringColorForLevel(level), 0.58f));
        const uint16_t unitColor = blendColor565(palette.muted, palette.background, 0.10f);

        drawLevelRing(canvas,
                      layout.homeDialCx,
                      layout.homeDialCy,
                      static_cast<int16_t>(layout.homeDialOuterRadius + 18),
                      static_cast<int16_t>(layout.homeDialOuterRadius + 38),
                      level,
                      blendColor565(palette.panel, palette.background, 0.36f),
                      ringAccent);
        if(!interactionActive) {
            drawDialTicks(canvas,
                          layout.homeDialCx,
                          layout.homeDialCy,
                          static_cast<int16_t>(layout.homeDialOuterRadius + 42),
                          static_cast<int16_t>(layout.homeDialOuterRadius + 52),
                          40,
                          blendColor565(palette.panelHi, palette.background, 0.24f));
        }
        canvas.fillCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius, palette.panel);
        canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius + 2, ringAccent);
        canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius + 7, blendColor565(palette.panelHi, kColorText, 0.16f));
        if(isRunningState() && homeWheelDisplayRpm() != 0) {
            drawHomeWheel(canvas, layout);
        }
        canvas.fillCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialInnerRadius, palette.background);
        canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialInnerRadius + 1, palette.panelHi);
        if(!interactionActive) {
            drawRoundHomeArcLabels(canvas, layout, palette);
        }

        canvas.drawCenteredText(fitTextToWidth(primaryValue,
                                               valueWidth,
                                               valueStyle,
                                               8),
                                layout.homeDialCx,
                                static_cast<int16_t>(layout.homeDialCy - 8),
                                valueStyle,
                                valueColor,
                                palette.background);
        canvas.drawCenteredText(fitTextToWidth(unitText,
                                               unitWidth,
                                               unitStyle,
                                               12),
                                layout.homeDialCx,
                                static_cast<int16_t>(layout.homeDialCy + 34),
                                unitStyle,
                                unitColor,
                                palette.background);

        return;
    }

    drawPill(canvas,
             layout.topLeft,
             isLinkOnline() ? "LINK" : "OFFLINE",
             isLinkOnline() ? kColorGood : kColorDanger,
             kColorBlack);
    drawPill(canvas,
             layout.topCenter,
             app.hasStatus ? app.status.drive : "?",
             palette.panel,
             kColorText);
    drawPill(canvas,
             layout.topRight,
             batteryPillLabel(app.hasStatus ? app.status.comm : "NO DATA"),
             batteryPillFill(palette.panel),
             batteryPillTextColor(kColorText));

    drawPill(canvas,
             layout.modeBadge,
             isGbwState() ? "GRIND BY WEIGHT" : "MANUAL RPM",
             palette.modeFill,
             kColorBlack);

    drawDialTicks(canvas,
                  layout.homeDialCx,
                  layout.homeDialCy,
                  static_cast<int16_t>(layout.homeDialOuterRadius + 6),
                  static_cast<int16_t>(layout.homeDialOuterRadius + 14),
                  isLargeCanvas() ? 24 : 18,
                  palette.panelHi);
    canvas.fillCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius, palette.panel);
    canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius + 2, palette.accent);
    canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialOuterRadius + 6, palette.panelHi);
    if(isRunningState() && homeWheelDisplayRpm() != 0) {
        drawHomeWheel(canvas, layout);
    }
    canvas.fillCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialInnerRadius, palette.background);
    canvas.drawCircle(layout.homeDialCx, layout.homeDialCy, layout.homeDialInnerRadius + 1, palette.panelHi);

    canvas.drawCenteredText(isGbwState() ? weightLabel(app.weightMg) : String(app.manualRpm),
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy - (isLargeCanvas() ? 22 : 12)),
                            heroTextStyle(),
                            kColorText,
                            palette.background);
    canvas.drawCenteredText(isGbwState() ? "Target dose" : "Set RPM",
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy + (isLargeCanvas() ? 28 : 18)),
                            bodyTextStyle(),
                            palette.muted,
                            palette.background);
    canvas.drawCenteredText("Drag the ring to adjust",
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy + (isLargeCanvas() ? 58 : 34)),
                            tinyTextStyle(),
                            palette.muted,
                            palette.background);

    drawButton(canvas,
               layout.homeModeButton,
               isGbwState() ? "MANUAL" : "GBW",
               stateAllowsModeToggle() ? palette.accent : palette.panel,
               stateAllowsModeToggle() ? kColorBlack : kColorMuted);
    drawButton(canvas,
               layout.homeActionButton,
               homeActionButtonLabel(),
               homeActionButtonFill(palette),
               homeActionButtonTextColor());

    const String stateLine = homePrimaryStatusLine();
    const String telemetryLine =
        String("RPM ") + (app.hasStatus ? String(homeWheelDisplayRpm()) : "-") +
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
                            palette.background);
    canvas.drawCenteredText(telemetryLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + (isLargeCanvas() ? 38 : 22)),
                            tinyTextStyle(),
                            palette.muted,
                            palette.background);
    canvas.drawCenteredText(detailLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + (isLargeCanvas() ? 58 : 36)),
                            tinyTextStyle(),
                            palette.muted,
                            palette.background);

    drawTabs(canvas, layout);
}

void drawMenuList(RemoteCanvas& canvas, const Layout& layout, const char* title) {
    canvas.fillScreen(kColorBg);

    if(useRoundNavigation()) {
        if(useWaveshareRoundUi()) {
            drawWaveshareRoundMenuList(canvas, layout, title);
            return;
        }

        size_t count = 0;
        const SettingItem* items = itemsForScreen(app.screen, &count);
        if(items == nullptr || count == 0) {
            drawPill(canvas, layout.modeBadge, title, kColorPanel, kColorText);
            drawFooter(canvas, layout, "No items");
            drawRoundNav(canvas, layout);
            return;
        }

        const uint8_t selected = selectionFor(app.screen);
        const size_t prevIdx = (selected + count - 1) % count;
        const size_t nextIdx = (selected + 1) % count;
        const SettingItem& current = items[selected];
        const String currentLabel = displayLabelForItem(current);
        const bool showValuePreview = current.kind == ItemKind::Value ||
                                      current.kind == ItemKind::Toggle ||
                                      isEspNowPeerSlotItem(current) ||
                                      isEspNowPeerScanItem(current) ||
                                      isEspNowPeerClearItem(current) ||
                                      isRadioModeItem(current) ||
                                      (current.action != nullptr && strcmp(current.action, "screen") == 0);
        const String currentValue = showValuePreview ? upperText(formatSettingValue(current)) : String("");
        const bool drawRingPreview = current.kind == ItemKind::Value;
        const float previewLevel = drawRingPreview ? normalizedItemLevel(current, menuValue(current)) : 0.0f;
        const Rect prevRect = shiftRectY(layout.menuRows[0], app.menuRollOffset);
        const Rect heroRect = shiftRectY(layout.menuRows[1], app.menuRollOffset);
        const Rect nextRect = shiftRectY(layout.menuRows[2], app.menuRollOffset);
        const int16_t cardCx = static_cast<int16_t>(heroRect.x + heroRect.w / 2);
        const int16_t cardCy = static_cast<int16_t>(heroRect.y + heroRect.h / 2 + 10);
        const int16_t ringOuter = clampI16(static_cast<int16_t>(heroRect.h * 0.29f), 74, 92);
        const int16_t ringInner = clampI16(static_cast<int16_t>(ringOuter - 30), 44, 62);
        const Rect currentRect = heroRect;
        const uint16_t previewAccent =
            current.kind == ItemKind::Danger ? kColorDanger :
            (current.kind == ItemKind::Action ? kColorAccent2 :
             (current.kind == ItemKind::Toggle ? blendColor565(kColorAccent, kColorPanelHi, 0.24f)
                                               : ringColorForLevel(previewLevel)));
        const uint16_t fadedText = blendColor565(kColorMuted, kColorBg, 0.56f);
        const uint16_t fadedFace = blendColor565(kColorPanel, kColorBg, 0.18f);
        const uint16_t fadedBorder = blendColor565(fadedFace, kColorBg, 0.08f);
        const uint16_t heroFace = blendColor565(kColorPanelHi, previewAccent, 0.24f);
        const uint16_t heroBorder = blendColor565(heroFace, kColorBg, 0.03f);
        const int16_t heroLabelWidth = static_cast<int16_t>(currentRect.w - 14);
        const int16_t heroValueWidth = static_cast<int16_t>(currentRect.w - 46);
        const RemoteTextStyle heroLabelStyle = chooseMenuHeroTextStyle(currentLabel, heroLabelWidth);
        const RemoteTextStyle heroValueStyle = chooseLabelTextStyle(currentValue, heroValueWidth);
        const RemoteTextStyle previewLabelStyle = RemoteTextStyle::Tiny;

        drawPill(canvas,
                 layout.modeBadge,
                 title,
                 kColorPanel,
                 kColorText);

        canvas.fillRoundRect(prevRect.x,
                             prevRect.y,
                             prevRect.w,
                             prevRect.h,
                             clampI16(static_cast<int16_t>(prevRect.h / 2), 8, 16),
                             fadedFace);
        drawSoftCenteredText(canvas,
                             fitTextToWidthNoTilde(displayLabelForItem(items[prevIdx]), static_cast<int16_t>(prevRect.w + 36), previewLabelStyle, 4),
                             static_cast<int16_t>(prevRect.x + prevRect.w / 2),
                             static_cast<int16_t>(prevRect.y + prevRect.h / 2),
                             previewLabelStyle,
                             blendColor565(kColorText, fadedText, 0.70f),
                             kColorBg,
                             fadedBorder,
                             true);

        drawNestCard(canvas,
                     currentRect,
                     heroFace,
                     heroBorder);
        if(drawRingPreview) {
            drawLevelRing(canvas,
                          cardCx,
                          cardCy,
                          static_cast<int16_t>(ringOuter + 14),
                          static_cast<int16_t>(ringOuter + 34),
                          previewLevel,
                          blendColor565(kColorPanel, kColorBg, 0.14f),
                          previewAccent);
        } else {
            drawDialTicks(canvas,
                          cardCx,
                          cardCy,
                          static_cast<int16_t>(ringOuter + 16),
                          static_cast<int16_t>(ringOuter + 34),
                          32,
                          blendColor565(previewAccent, kColorBg, 0.26f));
        }
        canvas.fillCircle(cardCx, cardCy, ringOuter, blendColor565(kColorPanel, previewAccent, 0.26f));
        canvas.drawCircle(cardCx, cardCy, static_cast<int16_t>(ringOuter + 2), blendColor565(heroFace, previewAccent, 0.10f));
        canvas.fillCircle(cardCx, cardCy, ringInner, kColorBg);
        canvas.drawCircle(cardCx, cardCy, static_cast<int16_t>(ringInner + 1), blendColor565(heroFace, kColorBg, 0.10f));
        drawSoftCenteredText(canvas,
                             fitTextToWidth(currentLabel, heroLabelWidth, heroLabelStyle, 18),
                             cardCx,
                             static_cast<int16_t>(cardCy - 10),
                             heroLabelStyle,
                             blendColor565(kColorText, previewAccent, 0.18f),
                             heroFace,
                             previewAccent);
        if(currentValue.length() > 0) {
            drawSoftCenteredText(canvas,
                                 fitTextToWidth(currentValue, heroValueWidth, heroValueStyle, 12),
                                 cardCx,
                                 static_cast<int16_t>(cardCy + ringInner - 10),
                                 heroValueStyle,
                                 blendColor565(previewAccent, kColorText, 0.34f),
                                 heroFace,
                                 previewAccent,
                                 true);
        }
        canvas.drawCenteredText(fitTextToWidth(String(""),
                                               static_cast<int16_t>(ringInner * 2),
                                               tinyTextStyle(),
                                               14),
                                cardCx,
                                static_cast<int16_t>(cardCy + 36),
                                tinyTextStyle(),
                                blendColor565(previewAccent, kColorText, 0.26f),
                                kColorBg);

        canvas.fillRoundRect(nextRect.x,
                             nextRect.y,
                             nextRect.w,
                             nextRect.h,
                             clampI16(static_cast<int16_t>(nextRect.h / 2), 8, 16),
                             fadedFace);
        drawSoftCenteredText(canvas,
                             fitTextToWidthNoTilde(displayLabelForItem(items[nextIdx]), static_cast<int16_t>(nextRect.w + 36), previewLabelStyle, 4),
                             static_cast<int16_t>(nextRect.x + nextRect.w / 2),
                             static_cast<int16_t>(nextRect.y + nextRect.h / 2),
                             previewLabelStyle,
                             blendColor565(kColorText, fadedText, 0.70f),
                             kColorBg,
                             fadedBorder,
                             true);
        const String footerText = (app.toastUntil > millis())
                                      ? upperText(app.toast)
                                      : upperText((current.kind == ItemKind::Action || current.kind == ItemKind::Danger)
                                                      ? "Tap center or short press"
                                                      : "Tap item or short press");
        drawFooter(canvas, layout, footerText);
        drawRoundNav(canvas, layout);
        return;
    }

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
        canvas.drawTextTopLeft(fitTextToWidth(items[idx].label,
                                              static_cast<int16_t>((layout.menuRows[row].w / 2) - layout.gap - 8),
                                              menuLabelTextStyle(),
                                              4),
                               static_cast<int16_t>(layout.menuRows[row].x + layout.gap + 4),
                               static_cast<int16_t>(layout.menuRows[row].y + layout.gap),
                               menuLabelTextStyle(),
                               textColor,
                               fill);
        canvas.drawTextTopRight(fitTextToWidth(formatSettingValue(items[idx]),
                                               static_cast<int16_t>((layout.menuRows[row].w / 2) - layout.gap - 8),
                                               menuValueTextStyle(),
                                               4),
                                static_cast<int16_t>(layout.menuRows[row].x + layout.menuRows[row].w - layout.gap - 4),
                                static_cast<int16_t>(layout.menuRows[row].y + layout.gap),
                                menuValueTextStyle(),
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
    const float level = normalizedItemLevel(item, app.draftValue);
    const uint16_t ringAccent = ringColorForLevel(level);

    if(useRoundNavigation()) {
        const String editorLabel = displayLabelForItem(item);
        const String editorValue = upperText(formatValueLabel(item, app.draftValue));
        const String editorHint = displayHintForItem(item);
        const int16_t editorLabelWidth = static_cast<int16_t>((layout.editorInnerRadius * 2) + 54);
        const int16_t editorValueWidth = static_cast<int16_t>((layout.editorInnerRadius * 2) + 54);
        const int16_t editorHintWidth = static_cast<int16_t>((layout.editorInnerRadius * 2) + 34);
        const RemoteTextStyle editorLabelStyle = chooseLabelTextStyle(editorLabel, editorLabelWidth);
        const RemoteTextStyle editorValueStyle = chooseValueTextStyle(editorValue, editorValueWidth);
        const RemoteTextStyle editorHintStyle = chooseLabelTextStyle(editorHint, editorHintWidth);

        drawPill(canvas, layout.modeBadge, title, kColorPanel, kColorText);

        if(usesChoiceEditor(item)) {
            Rect offRect = {};
            Rect onRect = {};
            buildToggleChoiceRects(layout, &offRect, &onRect);
            const bool enabled = app.draftValue != 0;

            canvas.drawCenteredText(fitTextToWidth(editorLabel,
                                                   static_cast<int16_t>(layout.width - (layout.pad * 6)),
                                                   editorLabelStyle,
                                                   16),
                                    layout.editorCx,
                                    static_cast<int16_t>(layout.editorCy - 96),
                                    editorLabelStyle,
                                    kColorMuted,
                                    kColorBg);
            canvas.drawCenteredText("Choose one",
                                    layout.editorCx,
                                    static_cast<int16_t>(layout.editorCy - 66),
                                    tinyTextStyle(),
                                    blendColor565(kColorMuted, kColorBg, 0.10f),
                                    kColorBg);
            drawChoiceAction(canvas,
                             offRect,
                             toggleOffLabel(item),
                             enabled ? blendColor565(kColorPanelHi, kColorBg, 0.08f)
                                     : blendColor565(kColorAccent2, kColorPanel, 0.24f),
                             enabled ? kColorMuted : kColorText,
                             !enabled);
            drawChoiceAction(canvas,
                             onRect,
                             toggleOnLabel(item),
                             enabled ? blendColor565(kColorGood, ringAccent, 0.22f)
                                     : blendColor565(kColorPanelHi, kColorBg, 0.08f),
                             enabled ? kColorBlack : kColorMuted,
                             enabled);
            drawFooter(canvas,
                       layout,
                       app.toastUntil > millis() ? upperText(app.toast)
                                                 : String("Tap choice or short press"));
        } else {
            drawLevelRing(canvas,
                          layout.editorCx,
                          layout.editorCy,
                          static_cast<int16_t>(layout.editorOuterRadius + 18),
                          static_cast<int16_t>(layout.editorOuterRadius + 38),
                          level,
                          blendColor565(kColorPanel, kColorBg, 0.24f),
                          ringAccent);
            drawDialTicks(canvas,
                          layout.editorCx,
                          layout.editorCy,
                          static_cast<int16_t>(layout.editorOuterRadius + 42),
                          static_cast<int16_t>(layout.editorOuterRadius + 52),
                          40,
                          blendColor565(kColorPanelHi, kColorBg, 0.28f));
            canvas.fillCircle(layout.editorCx,
                              layout.editorCy,
                              layout.editorOuterRadius,
                              blendColor565(kColorPanel, ringAccent, 0.22f));
            canvas.drawCircle(layout.editorCx, layout.editorCy, layout.editorOuterRadius + 3, ringAccent);
            canvas.drawCircle(layout.editorCx,
                              layout.editorCy,
                              layout.editorOuterRadius + 8,
                              blendColor565(ringAccent, kColorText, 0.14f));
            canvas.fillCircle(layout.editorCx, layout.editorCy, layout.editorInnerRadius, kColorBg);
            canvas.drawCircle(layout.editorCx,
                              layout.editorCy,
                              static_cast<int16_t>(layout.editorInnerRadius + 1),
                              blendColor565(ringAccent, kColorText, 0.18f));

            canvas.drawCenteredText(fitTextToWidth(editorLabel,
                                                   editorLabelWidth,
                                                   editorLabelStyle,
                                                   16),
                                    layout.editorCx,
                                    static_cast<int16_t>(layout.editorCy - 30),
                                    editorLabelStyle,
                                    kColorMuted,
                                    kColorBg);
            canvas.drawCenteredText(fitTextToWidth(editorValue,
                                                   editorValueWidth,
                                                   editorValueStyle,
                                                   12),
                                    layout.editorCx,
                                    layout.editorCy,
                                    editorValueStyle,
                                    kColorText,
                                    kColorBg);
            canvas.drawCenteredText(fitTextToWidth(editorHint,
                                                   editorHintWidth,
                                                   editorHintStyle,
                                                   16),
                                    layout.editorCx,
                                    static_cast<int16_t>(layout.editorCy + 34),
                                    editorHintStyle,
                                    blendColor565(ringAccent, kColorText, 0.26f),
                                    kColorBg);
            drawFooter(canvas,
                       layout,
                       app.toastUntil > millis() ? upperText(app.toast)
                                                 : String("Tap center or short press"));
        }
        return;
    }

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
            canvas.drawCenteredText(usesChoiceEditor(item) ? "Turn encoder to choose"
                                                          : "Drag the ring to adjust",
                                    layout.editorCx,
                                    static_cast<int16_t>(layout.editorCy + (isLargeCanvas() ? 44 : 28)),
                                    bodyTextStyle(),
                                    kColorMuted,
                            kColorBg);

    drawButton(canvas, layout.editCancel, "CANCEL", kColorPanel, kColorMuted);
    drawButton(canvas, layout.editSave, "SAVE", kColorGood, kColorBlack);
    if(!useRoundNavigation()) {
        drawTabs(canvas, layout);
    }
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
            drawEditor(canvas, layout, "GRINDER");
        } else if(app.screen == Screen::Network) {
            drawEditor(canvas, layout, "NETWORK");
        } else if(app.screen == Screen::EspNowPeers) {
            drawEditor(canvas, layout, "ESP-NOW");
        } else {
            drawEditor(canvas, layout, "DRIVE");
        }
    } else if(app.screen == Screen::Main) {
        drawMenuList(canvas, layout, "SYSTEM");
    } else if(app.screen == Screen::Purge) {
        drawMenuList(canvas, layout, "PURGE");
    } else if(app.screen == Screen::Gbw) {
        drawMenuList(canvas, layout, "GRINDER");
    } else if(app.screen == Screen::AudioDiag) {
        drawMenuList(canvas, layout, "AUDIO");
    } else if(app.screen == Screen::Network) {
        drawMenuList(canvas, layout, "NETWORK");
    } else if(app.screen == Screen::Drive) {
        drawMenuList(canvas, layout, "DRIVE");
    } else {
        drawMenuList(canvas, layout, "ESP-NOW");
    }

    canvas.present();
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
    app.wsDiscoveryPending = false;
    app.wsStaConnected = false;
    app.wsHost = "";
    app.wsEndpointLabel = "";
    stopMdnsClient();
}

void stopAllTransportStacks() {
    stopWebSocketSession();
    if(remote_wifi_is_active()) {
        remote_wifi_stop();
    }
    stopEspNowSession();
    app.lastLinkActivityAt = 0;
    app.currentTransport = LinkTransport::None;
    app.pendingTransport = LinkTransport::None;
    app.pendingTransportAt = 0;
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

    if(app.wsClientStarted) {
        wsClient.disconnect();
    }
    app.wsConnected = false;
    app.wsHost = host;
    app.wsPort = port;
    app.wsEndpointLabel = String("WS ") + host;
    wsClient.begin(app.wsHost.c_str(), app.wsPort, "/");
    wsClient.setReconnectInterval(remote_config::kWebSocketReconnectMs);
    app.wsClientStarted = true;
    app.lastWebSocketServiceAt = 0;
    wsClient.loop();
    Serial.printf("[remote_ws] connect %s:%u\n",
                  app.wsHost.c_str(),
                  static_cast<unsigned>(app.wsPort));
}

bool discoverControllerService(bool force) {
    if(!remote_wifi_sta_connected()) {
        return false;
    }
    ensureMdnsClientReady();
    if(!app.mdnsReady) {
        return false;
    }
    if(!force && (app.lastMdnsQueryAt + remote_config::kMdnsQueryMs) > millis()) {
        return false;
    }

    app.lastMdnsQueryAt = millis();
    const int serviceCount = MDNS.queryService("sg98", "tcp");
    if(serviceCount <= 0) {
        return false;
    }

    for(int i = 0; i < serviceCount; ++i) {
        const IPAddress ip = MDNS.address(i);
        const uint16_t port = MDNS.port(i);
        if(ip[0] == 0 || port == 0) {
            continue;
        }
        connectWebSocketHost(ip.toString(), port);
        return true;
    }
    return false;
}

void requestWebSocketDiscovery(bool resetSession) {
    if(resetSession) {
        if(app.wsClientStarted || app.wsConnected) {
            wsClient.disconnect();
        }
        app.wsConnected = false;
        app.wsClientStarted = false;
        app.wsHost = "";
        app.wsPort = remote_config::kWebSocketPort;
        app.wsEndpointLabel = "WebSocket scan";
    }
    app.wsDiscoveryPending = true;
    app.lastMdnsQueryAt = 0;
    app.lastWebSocketServiceAt = 0;
}

bool ensureEspNowPeer(const uint8_t* mac) {
    if(mac == nullptr) {
        return false;
    }

    if(esp_now_is_peer_exist(mac)) {
        return true;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
    peer.channel = remote_config::kEspNowChannel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

bool commandNeedsReliableEspNowSend(const String& command) {
    return command != sg98_protocol::kCmdGetStatus &&
           !command.startsWith("GET_MENU=") &&
           command != sg98_protocol::kCmdDiscover;
}

bool parseSetMenuTarget(const String& command, uint8_t* menuNumOut) {
    if(menuNumOut == nullptr || !command.startsWith("SET_MENU=")) {
        return false;
    }

    const int comma = command.indexOf(',');
    if(comma <= static_cast<int>(strlen("SET_MENU="))) {
        return false;
    }
    *menuNumOut = static_cast<uint8_t>(command.substring(strlen("SET_MENU="), comma).toInt());
    return true;
}

bool waitForEspNowCommandConfirmation(const String& command,
                                      uint32_t startedAckAt,
                                      uint32_t startedStatusAt,
                                      uint32_t startedMenuAt,
                                      uint8_t menuNum,
                                      uint32_t timeoutMs) {
    (void)command;
    const uint32_t startedAt = millis();
    while((millis() - startedAt) < timeoutMs) {
        if(app.lastAckAt != startedAckAt) {
            return true;
        }
        if(app.lastStatusAt != startedStatusAt) {
            return true;
        }
        if(menuNum < 5 && app.lastMenuRxAt[menuNum] != startedMenuAt) {
            return true;
        }
        delay(1);
    }
    return app.lastAckAt != startedAckAt ||
           app.lastStatusAt != startedStatusAt ||
           (menuNum < 5 && app.lastMenuRxAt[menuNum] != startedMenuAt);
}

void refreshEspNowSessionForRetry(const uint8_t* mac) {
    stopEspNowSession();
    if(!initEspNow()) {
        return;
    }
    if(mac != nullptr) {
        (void)ensureEspNowPeer(mac);
    }
}

bool sendEspNowFrame(const uint8_t* mac, const String& command) {
    if(!app.espNowReady) {
        return false;
    }
    if(mac == nullptr || command.length() == 0) {
        return false;
    }
    if(!ensureEspNowPeer(mac)) {
        refreshEspNowSessionForRetry(mac);
        if(!ensureEspNowPeer(mac)) {
            return false;
        }
    }

    app.sendComplete = false;
    app.sendOk = false;
    const esp_err_t rc = esp_now_send(mac,
                                      reinterpret_cast<const uint8_t*>(command.c_str()),
                                      command.length() + 1);
    return rc == ESP_OK;
}

bool sendEspNowRaw(const String& command) {
    return sendEspNowFrame(activeEspNowPeerMac(), command);
}

void requestEspNowDiscovery(bool force) {
    if(!app.espNowReady) {
        return;
    }

    const uint32_t now = millis();
    if(!force && (now - app.lastEspNowDiscoveryAt) < kEspNowDiscoveryIntervalMs) {
        return;
    }
    app.lastEspNowDiscoveryAt = now;
    sendEspNowFrame(kEspNowBroadcastMac, sg98_protocol::discoverRequest());
}

bool sendWebSocketRaw(const String& command) {
    if(!app.wsConnected) {
        return false;
    }

    String payload = command;
    wsClient.sendTXT(payload);
    return true;
}

bool sendRaw(const String& command) {
    if(kDisableTransportPerfTest) {
        (void)command;
        return false;
    }

    remoteConsoleTraceFrame("tx", command);
    switch(app.currentTransport) {
        case LinkTransport::WebSocket:
            return sendWebSocketRaw(command);
        case LinkTransport::EspNow:
            return sendEspNowRaw(command);
        default:
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

void requestAllMenus() {
    for(uint8_t menuNum = 1; menuNum <= 4; ++menuNum) {
        requestMenu(menuNum);
    }
}

// Rehydrate the remote from the controller whenever a transport comes up so
// stale locally cached menu values do not survive between sessions.
void requestFullControllerRefresh() {
    requestStatus();
    requestAllMenus();
}

void requestFullControllerRefreshOnLinkOnline(bool wasOnline) {
    if(wasOnline) {
        return;
    }
    clearPollSchedule();
    requestFullControllerRefresh();
}

void requestVisibleMenuIfNeeded(bool force) {
    auto requestIfDue = [&](uint8_t menuNum) {
        if(menuNum == 0) {
            return;
        }
        if(force || millis() - app.lastMenuPollAt[menuNum] >= remote_config::kVisibleMenuPollMs) {
            requestMenu(menuNum);
        }
    };

    switch(app.screen) {
        case Screen::Main:
            requestIfDue(1);
            break;
        case Screen::Purge:
            requestIfDue(2);
            break;
        case Screen::Gbw:
            requestIfDue(1);
            requestIfDue(3);
            break;
        case Screen::Network:
            requestIfDue(1);
            break;
        default:
            break;
    }
}

bool sendAction(const char* actionName, const char* arg0 = nullptr) {
    return sendRaw(sg98_protocol::action(actionName, arg0));
}

bool pendingTargetSatisfied(const PendingTargetWrite& pending) {
    if(!pending.active || !app.hasControllerStatus) {
        return false;
    }
    return pending.weightMode ? app.controllerStatus.setWeightMg == pending.value
                              : app.controllerStatus.setRpm == pending.value;
}

bool pendingMenuWriteSatisfied(const PendingMenuWrite& pending) {
    return pending.active &&
           pending.menuNum < 5 &&
           app.controllerMenuSynced[pending.menuNum] &&
           app.controllerMenuValues[pending.menuNum][pending.itemIdx] == pending.value;
}

bool pendingActionSatisfied(const PendingAction& pending) {
    if(pending.kind == PendingActionKind::None) {
        return false;
    }
    if(pending.reflection == PendingActionReflection::None) {
        return pending.attempts > 0;
    }
    if(!app.hasControllerStatus) {
        return false;
    }

    switch(pending.reflection) {
        case PendingActionReflection::State:
            return app.controllerStatus.state == pending.expectedText;
        case PendingActionReflection::Drive:
            return app.controllerStatus.drive == pending.expectedText;
        case PendingActionReflection::JmcBaud:
            return app.controllerStatus.jmcBaud == pending.value;
        case PendingActionReflection::None:
        default:
            return false;
    }
}

bool pendingActionSent(const PendingAction& pending) {
    return pending.attempts > 0;
}

void clearPendingTargetWrite() {
    app.pendingTarget = {};
}

void clearPendingMenuWrite(uint8_t slot) {
    if(slot >= kPendingMenuWriteSlots) {
        return;
    }
    app.pendingMenuWrites[slot] = {};
}

void clearPendingAction() {
    app.pendingAction = {};
}

bool pendingControllerSyncActive() {
    if(app.pendingTarget.active || app.pendingAction.kind != PendingActionKind::None) {
        return true;
    }
    for(const PendingMenuWrite& pending : app.pendingMenuWrites) {
        if(pending.active) {
            return true;
        }
    }
    return false;
}

int32_t normalizeTargetValue(bool weightMode, int32_t requestedValue) {
    if(weightMode) {
        return clampValue(requestedValue, static_cast<int32_t>(default_min_weight), static_cast<int32_t>(default_max_weight));
    }

    const int32_t minRpm = app.menuValues[1][SETMIN] > 0 ? app.menuValues[1][SETMIN] : default_minRPM;
    int32_t maxRpm = app.menuValues[1][SETMAX] > minRpm ? app.menuValues[1][SETMAX] : default_maxRPM;
    if(maxRpm < minRpm) {
        maxRpm = minRpm;
    }
    return clampValue(requestedValue, minRpm, maxRpm);
}

int32_t normalizeMenuValueForController(uint8_t menuNum, uint8_t itemIdx, int32_t requestedValue) {
    switch(menuNum) {
        case 1:
            if(itemIdx == SETMAX) {
                const int32_t minValue = app.menuValues[1][SETMIN];
                return max<int32_t>(requestedValue, minValue);
            }
            if(itemIdx == SETMIN) {
                const int32_t maxValue = app.menuValues[1][SETMAX];
                return min<int32_t>(requestedValue, maxValue);
            }
            break;
        case 3:
            if(itemIdx == GBW_RPM_SET) {
                int32_t slowLimit = app.menuValues[3][GBW_SLOW_RPM];
                if(slowLimit > app.menuValues[1][SETMAX]) {
                    slowLimit = app.menuValues[1][SETMAX];
                }
                int32_t maxValue = app.menuValues[1][SETMAX];
                if(maxValue < slowLimit) {
                    maxValue = slowLimit;
                }
                return clampValue(requestedValue, slowLimit, maxValue);
            }
            if(itemIdx == GBW_SLOW_RPM) {
                int32_t maxValue = app.menuValues[1][SETMAX];
                if(app.menuValues[3][GBW_RPM_SET] < maxValue) {
                    maxValue = app.menuValues[3][GBW_RPM_SET];
                }
                return min<int32_t>(requestedValue, maxValue);
            }
            break;
        default:
            break;
    }
    return requestedValue;
}

PendingMenuWrite* stagePendingMenuWrite(uint8_t menuNum, uint8_t itemIdx, int32_t value) {
    PendingMenuWrite* freeSlot = nullptr;
    PendingMenuWrite* oldestSlot = nullptr;
    for(uint8_t i = 0; i < kPendingMenuWriteSlots; ++i) {
        PendingMenuWrite& slot = app.pendingMenuWrites[i];
        if(slot.active && slot.menuNum == menuNum && slot.itemIdx == itemIdx) {
            slot.value = value;
            slot.queuedAt = millis();
            slot.lastSendAt = 0;
            slot.attempts = 0;
            return &slot;
        }
        if(!slot.active && freeSlot == nullptr) {
            freeSlot = &slot;
        }
        if(slot.active && (oldestSlot == nullptr || slot.queuedAt < oldestSlot->queuedAt)) {
            oldestSlot = &slot;
        }
    }

    PendingMenuWrite* slot = freeSlot != nullptr ? freeSlot : oldestSlot;
    if(slot == nullptr) {
        return nullptr;
    }

    *slot = {};
    slot->active = true;
    slot->menuNum = menuNum;
    slot->itemIdx = itemIdx;
    slot->value = value;
    slot->queuedAt = millis();
    return slot;
}

PendingTargetWrite* stagePendingTargetWrite(bool weightMode, int32_t value) {
    app.pendingTarget.active = true;
    app.pendingTarget.weightMode = weightMode;
    app.pendingTarget.value = value;
    app.pendingTarget.queuedAt = millis();
    app.pendingTarget.lastSendAt = 0;
    app.pendingTarget.attempts = 0;
    return &app.pendingTarget;
}

const char* pendingActionName(PendingActionKind kind) {
    switch(kind) {
        case PendingActionKind::Start: return "start";
        case PendingActionKind::Stop: return "stop";
        case PendingActionKind::Idle: return "idle";
        case PendingActionKind::IdleGbw: return "idle_gbw";
        case PendingActionKind::Calibrate: return "calibrate";
        case PendingActionKind::DriveAuto:
        case PendingActionKind::DriveJmc:
        case PendingActionKind::DriveRt: return "drive";
        case PendingActionKind::JmcBaud: return "jmc_baud";
        case PendingActionKind::ResetDefaults: return "reset_defaults";
        case PendingActionKind::None:
        default: return nullptr;
    }
}

void stagePendingAction(PendingActionKind kind,
                        PendingActionReflection reflection,
                        const char* arg0,
                        const char* expectedText,
                        int32_t expectedValue) {
    app.pendingAction = {};
    app.pendingAction.kind = kind;
    app.pendingAction.reflection = reflection;
    app.pendingAction.value = expectedValue;
    app.pendingAction.queuedAt = millis();
    if(arg0 != nullptr) {
        copyStringToBuffer(String(arg0), app.pendingAction.arg0);
    }
    if(expectedText != nullptr) {
        copyStringToBuffer(String(expectedText), app.pendingAction.expectedText);
    }
}

bool pendingCommandRetryDue(uint32_t lastSendAt, uint32_t now) {
    return lastSendAt == 0 || (now - lastSendAt) >= remote_config::kPendingSyncRetryMs;
}

bool trySendPendingTargetWrite(PendingTargetWrite* pending, bool force = false) {
    if(pending == nullptr || !pending->active) {
        return false;
    }
    const uint32_t now = millis();
    if(!force && !pendingCommandRetryDue(pending->lastSendAt, now)) {
        return false;
    }
    const bool sent = pending->weightMode ? sendRaw(sg98_protocol::setWeightMg(pending->value))
                                          : sendRaw(sg98_protocol::setRpm(pending->value));
    if(sent) {
        pending->lastSendAt = now;
        ++pending->attempts;
        app.lastStatusPollAt = 0;
    }
    return sent;
}

bool trySendPendingMenuWrite(PendingMenuWrite* pending, bool force = false) {
    if(pending == nullptr || !pending->active) {
        return false;
    }
    const uint32_t now = millis();
    if(!force && !pendingCommandRetryDue(pending->lastSendAt, now)) {
        return false;
    }
    const bool sent = sendRaw(sg98_protocol::setMenuValue(pending->menuNum, pending->itemIdx, pending->value));
    if(sent) {
        pending->lastSendAt = now;
        ++pending->attempts;
        app.lastMenuPollAt[pending->menuNum] = 0;
        app.lastStatusPollAt = 0;
    }
    return sent;
}

bool trySendPendingAction(PendingAction* pending, bool force = false) {
    if(pending == nullptr || pending->kind == PendingActionKind::None) {
        return false;
    }
    const uint32_t now = millis();
    if(!force && !pendingCommandRetryDue(pending->lastSendAt, now)) {
        return false;
    }
    const char* actionName = pendingActionName(pending->kind);
    if(actionName == nullptr) {
        return false;
    }
    const char* arg0 = pending->arg0[0] != '\0' ? pending->arg0 : nullptr;
    const bool sent = sendAction(actionName, arg0);
    if(sent) {
        pending->lastSendAt = now;
        ++pending->attempts;
        app.lastStatusPollAt = 0;
    }
    return sent;
}

bool queuePendingAction(PendingActionKind kind,
                        PendingActionReflection reflection,
                        const char* arg0,
                        const char* expectedText,
                        int32_t expectedValue,
                        bool* sentNow = nullptr) {
    stagePendingAction(kind, reflection, arg0, expectedText, expectedValue);
    const bool sent = trySendPendingAction(&app.pendingAction, true);
    if(sentNow != nullptr) {
        *sentNow = sent;
    }
    return sent || app.pendingAction.kind != PendingActionKind::None;
}

void applyPendingUiOverridesToStatusFrame(sg98_protocol::StatusFrame* status) {
    if(status == nullptr) {
        return;
    }

    if(app.pendingTarget.active) {
        if(app.pendingTarget.weightMode) {
            status->setWeightMg = app.pendingTarget.value;
        } else {
            status->setRpm = app.pendingTarget.value;
        }
    }

    if(app.pendingAction.kind != PendingActionKind::None &&
       app.pendingAction.reflection == PendingActionReflection::State &&
       pendingActionSent(app.pendingAction) &&
       !pendingActionSatisfied(app.pendingAction)) {
        status->state = String(app.pendingAction.expectedText);
    }
}

void applyPendingUiOverridesToMenu(uint8_t menuNum) {
    if(menuNum >= 5) {
        return;
    }

    for(const PendingMenuWrite& pending : app.pendingMenuWrites) {
        if(!pending.active || pending.menuNum != menuNum) {
            continue;
        }
        app.menuValues[menuNum][pending.itemIdx] = pending.value;
    }
}

void reconcilePendingControllerSync() {
    if(pendingTargetSatisfied(app.pendingTarget)) {
        clearPendingTargetWrite();
    }

    for(uint8_t i = 0; i < kPendingMenuWriteSlots; ++i) {
        if(pendingMenuWriteSatisfied(app.pendingMenuWrites[i])) {
            clearPendingMenuWrite(i);
        }
    }

    if(pendingActionSatisfied(app.pendingAction)) {
        clearPendingAction();
    }
}

void servicePendingMenuPolls() {
    bool requestedMenu[5] = {};
    const uint32_t now = millis();
    for(const PendingMenuWrite& pending : app.pendingMenuWrites) {
        if(!pending.active || pending.menuNum >= 5 || requestedMenu[pending.menuNum]) {
            continue;
        }
        if((now - app.lastMenuPollAt[pending.menuNum]) >= remote_config::kPendingMenuPollMs) {
            requestMenu(pending.menuNum);
            requestedMenu[pending.menuNum] = true;
        }
    }
}

void servicePendingControllerSync() {
    reconcilePendingControllerSync();
    if(!pendingControllerSyncActive()) {
        return;
    }

    if(app.pendingAction.kind != PendingActionKind::None && !pendingActionSatisfied(app.pendingAction)) {
        if(trySendPendingAction(&app.pendingAction)) {
            return;
        }
    }

    if(app.pendingTarget.active && !pendingTargetSatisfied(app.pendingTarget)) {
        if(trySendPendingTargetWrite(&app.pendingTarget)) {
            return;
        }
    }

    for(uint8_t offset = 0; offset < kPendingMenuWriteSlots; ++offset) {
        const uint8_t slotIdx = static_cast<uint8_t>((app.pendingMenuCursor + offset) % kPendingMenuWriteSlots);
        PendingMenuWrite& pending = app.pendingMenuWrites[slotIdx];
        if(!pending.active || pendingMenuWriteSatisfied(pending)) {
            continue;
        }
        if(trySendPendingMenuWrite(&pending)) {
            app.pendingMenuCursor = static_cast<uint8_t>((slotIdx + 1) % kPendingMenuWriteSlots);
            return;
        }
    }
}

void switchTransportNow(LinkTransport transport) {
    if(kDisableTransportPerfTest) {
        (void)transport;
        stopAllTransportStacks();
        app.lastTransportSwitchAt = millis();
        clearPollSchedule();
        return;
    }

    if(transport == LinkTransport::None) {
        transport = initialTransport();
    }

    app.pendingTransport = LinkTransport::None;
    app.pendingTransportAt = 0;
    if(app.currentTransport == transport) {
        return;
    }

    app.lastLinkActivityAt = 0;

    if(transport == LinkTransport::WebSocket) {
        stopEspNowSession();
        if(!remote_wifi_is_active()) {
            remote_wifi_start();
        }
        app.wsStaConnected = remote_wifi_sta_connected();
        app.wsDiscoveryPending = remote_wifi_has_saved_network();
        if(remote_wifi_has_saved_network()) {
            setToast(remote_wifi_sta_connected() ? "WebSocket mode" : "Connecting Wi-Fi", 1200);
        } else {
            app.toastUntil = 0;
        }
    } else if(transport == LinkTransport::EspNow) {
        stopWebSocketSession();
        if(remote_wifi_is_active()) {
            remote_wifi_stop();
        }
        app.wsDiscoveryPending = false;
        app.wsStaConnected = false;
        if(initEspNow()) {
            setToast("ESP-NOW mode", 1200);
        }
    }

    app.currentTransport = transport;
    app.lastTransportSwitchAt = millis();
    clearPollSchedule();
    markPersistentStateDirty();
}

void applyPendingTransportSwitch() {
    if(app.pendingTransport == LinkTransport::None || millis() < app.pendingTransportAt) {
        return;
    }

    switchTransportNow(app.pendingTransport);
}

LinkTransport alternateTransport() {
    if(app.transportOverrideActive) {
        return app.currentTransport;
    }

    if(app.radioModeKnown) {
        const LinkTransport expected = transportFromRadioMode(app.menuValues[1][RADIOMODE]);
        if(expected == LinkTransport::EspNow) {
            return hasActiveEspNowPeer() ? LinkTransport::EspNow : app.currentTransport;
        }
        if(expected == LinkTransport::WebSocket) {
            return remote_wifi_has_saved_network() ? LinkTransport::WebSocket : app.currentTransport;
        }
    }

    if(app.currentTransport == LinkTransport::WebSocket) {
        if(hasActiveEspNowPeer()) {
            return LinkTransport::EspNow;
        }
        return app.currentTransport;
    }

    if(app.currentTransport == LinkTransport::EspNow) {
        if(app.screen == Screen::EspNowPeers || !hasActiveEspNowPeer()) {
            return app.currentTransport;
        }
        if(webSocketCandidateAvailable()) {
            return LinkTransport::WebSocket;
        }
        return app.currentTransport;
    }

    return initialTransport();
}

void consumeStatusFrame(const sg98_protocol::StatusFrame& status) {
    clearRecoveredSendFailureToast();
    const bool wasOnline = isLinkOnline();
    const bool hadStatus = app.hasStatus;
    const sg98_protocol::StatusFrame previous = app.status;
    const bool previousPreferGbw = app.preferGbw;
    const int32_t previousManualRpm = app.manualRpm;
    const int32_t previousWeightMg = app.weightMg;
    app.controllerStatus = status;
    app.hasControllerStatus = true;
    sg98_protocol::StatusFrame displayStatus = status;
    applyPendingUiOverridesToStatusFrame(&displayStatus);
    const bool valuesChanged = !hadStatus || !statusFrameEqual(app.status, displayStatus);
    app.status = displayStatus;
    app.hasStatus = true;
    app.lastLinkActivityAt = millis();
    app.lastStatusAt = millis();
    app.preferGbw = app.status.state == "IDLE_GBW" || app.status.state == "GRINDING_GBW";
    if(!app.targetDirty && !app.pendingTarget.active) {
        app.manualRpm = status.setRpm;
        app.weightMg = status.setWeightMg;
    }
    if(previousPreferGbw != app.preferGbw ||
       previousManualRpm != app.manualRpm ||
       previousWeightMg != app.weightMg) {
        markPersistentStateDirty();
    }
    reconcilePendingControllerSync();
    if(shouldRedrawForStatusFrame(previous, app.status, hadStatus, valuesChanged)) {
        app.redraw = true;
    }
    requestFullControllerRefreshOnLinkOnline(wasOnline);
}

void consumeMenuFrame(const sg98_protocol::MenuFrame& frame) {
    if(frame.menuNum >= 5) {
        return;
    }

    clearRecoveredSendFailureToast();
    const bool wasOnline = isLinkOnline();
    const bool valuesChanged = !menuFrameMatchesCache(frame);
    const bool wasSynced = app.menuSynced[frame.menuNum];
    const bool previousRadioModeKnown = app.radioModeKnown;
    for(uint8_t i = 0; i < frame.count && i < sg98_protocol::kMaxMenuValues; ++i) {
        app.controllerMenuValues[frame.menuNum][i] = frame.values[i];
        app.menuValues[frame.menuNum][i] = frame.values[i];
    }
    app.controllerMenuSynced[frame.menuNum] = true;
    app.menuSynced[frame.menuNum] = true;
    applyPendingUiOverridesToMenu(frame.menuNum);
    app.lastLinkActivityAt = millis();
    app.lastMenuRxAt[frame.menuNum] = millis();

    if(frame.menuNum == 1) {
        if(valuesChanged || !wasSynced) {
            applyCurrentBacklightLevel();
        }
        app.radioModeKnown = true;
        app.transportOverrideActive = false;
        const LinkTransport expected = transportFromRadioMode(app.menuValues[1][RADIOMODE]);
        if(expected != app.currentTransport) {
            scheduleTransportSwitch(expected);
        }
    }

    if(valuesChanged || !wasSynced || previousRadioModeKnown != app.radioModeKnown) {
        markPersistentStateDirty();
    }
    reconcilePendingControllerSync();
    if(shouldRedrawForMenuFrame(frame, valuesChanged)) {
        app.redraw = true;
    }
    requestFullControllerRefreshOnLinkOnline(wasOnline);
}

void upsertDiscoveredEspNowPeer(const sg98_protocol::PeerFrame& frame) {
    uint8_t mac[6] = {};
    if(!parseMacAddress(frame.mac, mac)) {
        return;
    }

    int targetIndex = -1;
    int freeIndex = -1;
    int oldestIndex = 0;
    uint32_t oldestSeenAt = UINT32_MAX;
    for(uint8_t i = 0; i < kEspNowPeerSlots; ++i) {
        if(app.espNowPeers[i].valid) {
            if(macAddressEqual(app.espNowPeers[i].mac, mac)) {
                targetIndex = i;
                break;
            }
            if(app.espNowPeers[i].seenAt < oldestSeenAt) {
                oldestSeenAt = app.espNowPeers[i].seenAt;
                oldestIndex = i;
            }
        } else if(freeIndex < 0) {
            freeIndex = i;
        }
    }

    if(targetIndex < 0) {
        targetIndex = freeIndex >= 0 ? freeIndex : oldestIndex;
    }

    DiscoveredEspNowPeer& peer = app.espNowPeers[targetIndex];
    peer.valid = true;
    memcpy(peer.mac, mac, sizeof(peer.mac));
    peer.channel = static_cast<uint8_t>(clampValue(frame.channel, 0, 255));
    peer.seenAt = millis();
    peer.name = frame.name;
    peer.target = frame.target;
    sanitizeSelections();
    app.redraw = app.redraw || app.screen == Screen::EspNowPeers;
}

void consumePeerFrame(const sg98_protocol::PeerFrame& frame) {
    clearRecoveredSendFailureToast();
    app.lastLinkActivityAt = millis();
    upsertDiscoveredEspNowPeer(frame);
}

void consumeProtocolFrame(const String& frame) {
    remoteConsoleTraceFrame("rx", frame);
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

    sg98_protocol::PeerFrame peerFrame;
    if(sg98_protocol::parsePeerFrame(frame, &peerFrame)) {
        consumePeerFrame(peerFrame);
        return;
    }

    sg98_protocol::AckFrame ackFrame;
    if(sg98_protocol::parseAckFrame(frame, &ackFrame)) {
        const bool wasOnline = isLinkOnline();
        app.lastLinkActivityAt = millis();
        app.lastAckAt = millis();
        if(ackFrame.ok) {
            clearRecoveredSendFailureToast();
        }
        requestFullControllerRefreshOnLinkOnline(wasOnline);
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
            Serial.printf("[remote_ws] connected %s:%u path=%.*s\n",
                          app.wsHost.c_str(),
                          static_cast<unsigned>(app.wsPort),
                          static_cast<int>(length),
                          payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
            clearPollSchedule();
            requestFullControllerRefresh();
            break;
        case WStype_DISCONNECTED:
            app.wsConnected = false;
            app.wsEndpointLabel = "WebSocket offline";
            Serial.printf("[remote_ws] disconnected len=%u payload=%.*s\n",
                          static_cast<unsigned>(length),
                          static_cast<int>(length),
                          payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
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
        case WStype_ERROR:
            Serial.printf("[remote_ws] error len=%u payload=%.*s\n",
                          static_cast<unsigned>(length),
                          static_cast<int>(length),
                          payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
            break;
        default:
            Serial.printf("[remote_ws] event=%d len=%u\n",
                          static_cast<int>(type),
                          static_cast<unsigned>(length));
            break;
    }
}

void onEspNowSent(const esp_now_send_info_t*, esp_now_send_status_t status) {
    app.sendOk = (status == ESP_NOW_SEND_SUCCESS);
    app.sendComplete = true;
}

void onEspNowReceive(const esp_now_recv_info_t*, const uint8_t* data, int len) {
    if(data == nullptr || len <= 0) {
        return;
    }

    consumeProtocolFrame(String(reinterpret_cast<const char*>(data)));
}

bool initEspNow() {
    if(app.espNowReady) {
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(!remote_config::kDisableWifiModemSleepForTesting);

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

    if(!ensureEspNowPeer(kEspNowBroadcastMac)) {
        esp_now_deinit();
        setToast("Add peer failed");
        return false;
    }

    const uint8_t* peerMac = activeEspNowPeerMac();
    if(peerMac != nullptr && !ensureEspNowPeer(peerMac)) {
        esp_now_deinit();
        setToast("Add lock failed");
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
    if(!app.sendOk && !pendingControllerSyncActive()) {
        setToast("Send failed");
    }
}

void updateSyncFromStatusFallback() {
    if(app.hasControllerStatus && !app.targetDirty && !app.pendingTarget.active) {
        app.manualRpm = app.controllerStatus.setRpm;
        app.weightMg = app.controllerStatus.setWeightMg;
    }
}

void manageTransport() {
    const uint32_t now = millis();
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
        if((now - app.lastWifiServiceAt) >= remote_config::kWifiServiceMs) {
            remote_wifi_poll();
            app.lastWifiServiceAt = now;
        }
        const bool staConnected = remote_wifi_sta_connected();
        if(staConnected != app.wsStaConnected) {
            app.wsStaConnected = staConnected;
            if(staConnected) {
                app.wsDiscoveryPending = true;
            }
        }
        if(staConnected) {
            ensureMdnsClientReady();
            if(!app.wsClientStarted && !app.wsHost.isEmpty()) {
                connectWebSocketHost(app.wsHost, app.wsPort);
            }
            if(app.wsClientStarted &&
               (now - app.lastWebSocketServiceAt) >= currentWebSocketLoopIntervalMs()) {
                wsClient.loop();
                app.lastWebSocketServiceAt = now;
            }
            if(app.wsDiscoveryPending) {
                (void)discoverControllerService(true);
                app.wsDiscoveryPending = false;
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
        if(app.screen == Screen::EspNowPeers || !hasActiveEspNowPeer()) {
            requestEspNowDiscovery(false);
        }
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
    if(millis() - app.lastStatusPollAt >= currentStatusPollIntervalMs()) {
        requestStatus();
        app.lastStatusPollAt = millis();
    }

    servicePendingMenuPolls();

    if(millis() - app.lastBackgroundMenuPollAt >= remote_config::kBackgroundMenuPollMs) {
        for(uint8_t menuNum = 1; menuNum <= 4; ++menuNum) {
            requestMenu(menuNum);
        }
        app.lastBackgroundMenuPollAt = millis();
    }

    if(!roundUiInteractionActive()) {
        requestVisibleMenuIfNeeded(false);
    }
}

bool applyMenuValue(const SettingItem& item, int32_t value) {
    if(isInvertScrollItem(item)) {
        app.localInvertScroll = clampValue(value, 0, 1);
        markPersistentStateDirty();
        return true;
    }
    if(isUiSoundsItem(item)) {
        app.localUiSounds = clampValue(value, 0, 1);
        markPersistentStateDirty();
        return true;
    }
    if(item.menuNum == 0) {
        return false;
    }

    const int32_t clampedValue = clampValue(value, item.minValue, item.maxValue);
    const int32_t normalizedValue = normalizeMenuValueForController(item.menuNum, item.itemIdx, clampedValue);
    const int32_t previousValue = app.menuValues[item.menuNum][item.itemIdx];
    const bool previousRadioModeKnown = app.radioModeKnown;
    app.menuValues[item.menuNum][item.itemIdx] = normalizedValue;
    applyLocalMenuPreview(item, normalizedValue);
    if(item.menuNum == 1 && item.itemIdx == RADIOMODE) {
        app.radioModeKnown = true;
        scheduleTransportSwitch(transportFromRadioMode(normalizedValue), remote_config::kTransportSwitchDelayMs);
    }
    PendingMenuWrite* pending = stagePendingMenuWrite(item.menuNum, item.itemIdx, normalizedValue);
    if(pending == nullptr) {
        app.menuValues[item.menuNum][item.itemIdx] = previousValue;
        applyLocalMenuPreview(item, previousValue);
        app.radioModeKnown = previousRadioModeKnown;
        app.pendingTransport = LinkTransport::None;
        app.pendingTransportAt = 0;
        return false;
    }
    (void)trySendPendingMenuWrite(pending, true);
    app.lastMenuPollAt[item.menuNum] = 0;
    app.lastStatusPollAt = 0;
    markPersistentStateDirty();
    return true;
}

void cancelEditing() {
    if(!app.editing) {
        return;
    }

    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items != nullptr && count > 0) {
        const SettingItem& item = items[selectionFor(app.screen)];
        applyLocalMenuPreview(item, menuValue(item));
    }
    app.editing = false;
    setToast("Edit canceled", 900);
}

void lockEspNowPeer(const DiscoveredEspNowPeer& peer) {
    setLockedEspNowPeer(peer.mac);
    if(app.espNowReady) {
        ensureEspNowPeer(peer.mac);
    }
    clearPollSchedule();
    requestFullControllerRefresh();
    markPersistentStateDirty();
    setToast(String("Locked ") + espNowPeerName(peer), 1200);
}

void runConfirmAction(const SettingItem& item) {
    if(item.action == nullptr) {
        return;
    }

    if(strcmp(item.action, "reset_network") == 0) {
        remote_wifi_reset_network_settings();
        clearPollSchedule();
        switchTransportNow(initialTransport());
        requestFullControllerRefresh();
        markPersistentStateDirty();
        setToast("Remote network reset", 1200);
        return;
    }

    bool queued = false;
    if(strcmp(item.action, "calibrate") == 0) {
        queued = queuePendingAction(PendingActionKind::Calibrate,
                                    PendingActionReflection::State,
                                    nullptr,
                                    "CALIBRATING",
                                    0);
    } else if(strcmp(item.action, "redetect") == 0) {
        queued = queuePendingAction(PendingActionKind::DriveAuto,
                                    PendingActionReflection::None,
                                    "auto",
                                    nullptr,
                                    0);
    } else if(strcmp(item.action, "drive") == 0 && item.actionArg != nullptr) {
        if(strcmp(item.actionArg, "jmc") == 0) {
            queued = queuePendingAction(PendingActionKind::DriveJmc,
                                        PendingActionReflection::Drive,
                                        "jmc",
                                        "JMC",
                                        0);
        } else if(strcmp(item.actionArg, "rt") == 0) {
            queued = queuePendingAction(PendingActionKind::DriveRt,
                                        PendingActionReflection::Drive,
                                        "rt",
                                        "RT",
                                        0);
        }
    } else if(strcmp(item.action, "jmc_baud") == 0 && item.actionArg != nullptr) {
        const int32_t baud = atoi(item.actionArg);
        queued = queuePendingAction(PendingActionKind::JmcBaud,
                                    PendingActionReflection::JmcBaud,
                                    item.actionArg,
                                    nullptr,
                                    baud);
    } else if(strcmp(item.action, "reset_defaults") == 0) {
        queued = queuePendingAction(PendingActionKind::ResetDefaults,
                                    PendingActionReflection::None,
                                    item.actionArg,
                                    nullptr,
                                    0);
    } else {
        queued = sendAction(item.action, item.actionArg);
    }

    if(queued) {
        app.lastStatusPollAt = 0;
        requestVisibleMenuIfNeeded(true);
        if(item.kind == ItemKind::Danger) {
            app.dangerArmUntil = 0;
        }
        setToast(String("Sent ") + item.label, 1000);
        return;
    }

    setToast(String("Action not sent"), 1000);
}

void activateSelectedItem() {
    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items == nullptr || count == 0) {
        return;
    }

    const uint8_t selected = selectionFor(app.screen);
    const SettingItem& item = items[selected];
    const bool returnToParentMenu = (app.screen == Screen::Drive);
    debugRoundInput("activate", selected, item.menuNum, item.itemIdx, static_cast<int32_t>(item.kind));

    if(item.kind == ItemKind::Value || (item.kind == ItemKind::Toggle && useRoundNavigation()) || isConfirmActionItem(item)) {
        if(!app.editing) {
            app.draftValue = isConfirmActionItem(item) ? 0 : menuValue(item);
            app.editing = true;
            setToast(isConfirmActionItem(item) ? String(item.label) + "?"
                                              : String("Edit ") + item.label,
                     900);
        } else {
            app.editing = false;
            if(isConfirmActionItem(item)) {
                if(app.draftValue != 0) {
                    runConfirmAction(item);
                } else {
                    setToast(String(item.label) + " canceled", 900);
                }
            } else {
                const int32_t nextValue = clampValue(app.draftValue, item.minValue, item.maxValue);
                if(applyMenuValue(item, nextValue)) {
                    if(isRadioModeItem(item) && nextValue != 0) {
                        setToast("Pick controller", 1200);
                        setScreen(Screen::EspNowPeers);
                        requestEspNowDiscovery(true);
                        return;
                    }
                    setToast(String("Saved ") + item.label);
                } else {
                    setToast("Send failed", 1000);
                }
            }
        }
        app.redraw = true;
        return;
    }

    if(item.kind == ItemKind::Toggle) {
        const int32_t nextValue = menuValue(item) == 0 ? 1 : 0;
        if(applyMenuValue(item, nextValue)) {
            if(app.localUiSounds != 0) {
                remote_audio_play(RemoteAudioTone::Click);
            }
            setToast(String(item.label) + ": " + formatValueLabel(item, nextValue));
        } else {
            setToast("Send failed", 1000);
        }
        app.redraw = true;
        return;
    }

    if(item.kind == ItemKind::Danger && !isConfirmActionItem(item) && !dangerIsArmed(app.screen, selected)) {
        armDanger(app.screen, selected);
        setToast("Press again to confirm", remote_config::kDangerConfirmMs);
        app.redraw = true;
        return;
    }

    if(item.action != nullptr && strcmp(item.action, "screen") == 0) {
        if(item.actionArg != nullptr && strcmp(item.actionArg, "drive") == 0) {
            setScreen(Screen::Drive);
        } else if(item.actionArg != nullptr && strcmp(item.actionArg, "audio") == 0) {
            setScreen(Screen::AudioDiag);
        } else if(isControllerLinkItem(item)) {
            if(controllerLinkUsesWebSocketScan()) {
                if(!remote_wifi_has_saved_network()) {
                    setToast("Set Wi-Fi first", 1200);
                } else {
                    requestWebSocketDiscovery(true);
                    setToast("Scanning controller", 1200);
                }
            } else {
                setScreen(Screen::EspNowPeers);
            }
        }
        app.redraw = true;
        return;
    }

    if(item.action != nullptr && strcmp(item.action, "audio_probe") == 0) {
        remote_audio_init();
        const RemoteAudioStatus status = remote_audio_status();
        setToast(status.codecDetected ? "Audio codec detected" : String("Audio: ") + remote_audio_detail(), 1400);
        app.redraw = true;
        return;
    }

    if(item.action != nullptr && strcmp(item.action, "audio_click") == 0) {
        const bool ok = remote_audio_play(RemoteAudioTone::Click);
        setToast(ok ? "Click tone" : String("Audio: ") + remote_audio_detail(), 1400);
        app.redraw = true;
        return;
    }

    if(item.action != nullptr && strcmp(item.action, "audio_swipe") == 0) {
        const bool ok = remote_audio_play(RemoteAudioTone::SwipeRight);
        setToast(ok ? "Swipe tone" : String("Audio: ") + remote_audio_detail(), 1400);
        app.redraw = true;
        return;
    }

    if(item.action != nullptr && strcmp(item.action, "audio_sweep") == 0) {
        const bool ok = remote_audio_play(RemoteAudioTone::Sweep);
        setToast(ok ? "Sweep tone" : String("Audio: ") + remote_audio_detail(), 1400);
        app.redraw = true;
        return;
    }

    if(isEspNowPeerSlotItem(item)) {
        const DiscoveredEspNowPeer* peer = visibleEspNowPeerAt(static_cast<uint8_t>(item.itemIdx));
        if(peer != nullptr) {
            lockEspNowPeer(*peer);
            setScreen(Screen::Network);
            return;
        }
        setToast("No controller", 900);
        app.redraw = true;
        return;
    }

    if(isEspNowPeerScanItem(item)) {
        requestEspNowDiscovery(true);
        setToast("Scanning controllers", 1200);
        app.redraw = true;
        return;
    }

    if(isEspNowPeerClearItem(item)) {
        clearLockedEspNowPeer();
        markPersistentStateDirty();
        setToast("Controller lock cleared", 1200);
        app.redraw = true;
        return;
    }

    if(item.action != nullptr && strcmp(item.action, "redetect") == 0) {
        setLocalDriveSelection(LOCAL_DRIVE_AUTO);
        markPersistentStateDirty();
    } else if(item.action != nullptr && strcmp(item.action, "drive") == 0 && item.actionArg != nullptr) {
        if(strcmp(item.actionArg, "jmc") == 0) {
            setLocalDriveSelection(LOCAL_DRIVE_JMC);
            markPersistentStateDirty();
        } else if(strcmp(item.actionArg, "rt") == 0) {
            setLocalDriveSelection(LOCAL_DRIVE_RT);
            markPersistentStateDirty();
        }
    }

    runConfirmAction(item);
    if(returnToParentMenu) {
        setScreen(Screen::Main);
        return;
    }
    app.redraw = true;
}

void setScreen(Screen screen) {
    if(app.screen == screen && !app.editing) {
        return;
    }

    app.screen = screen;
    app.menuRollOffset = 0.0f;
    app.editing = false;
    app.redraw = true;
    requestVisibleMenuIfNeeded(true);
    sanitizeSelections();
    if(screen == Screen::EspNowPeers && app.currentTransport == LinkTransport::EspNow) {
        requestEspNowDiscovery(true);
    }
    if(app.localUiSounds != 0) {
        remote_audio_play(RemoteAudioTone::Click);
    }
    markPersistentStateDirty();
}

void commitTargetIfDue() {
    if(!app.targetDirty || millis() < app.targetCommitAt) {
        return;
    }

    const bool weightMode = isGbwState();
    const int32_t normalizedValue = normalizeTargetValue(weightMode, weightMode ? app.weightMg : app.manualRpm);
    if(weightMode) {
        app.weightMg = normalizedValue;
    } else {
        app.manualRpm = normalizedValue;
    }
    PendingTargetWrite* pending = stagePendingTargetWrite(weightMode, normalizedValue);
    (void)trySendPendingTargetWrite(pending, true);
    app.targetDirty = false;
    app.lastStatusPollAt = 0;
}

void moveSelection(int delta, bool animate = true) {
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
    debugRoundInput("select", delta, selected, static_cast<int32_t>(count), lroundf(app.menuRollOffset));
    if(useRoundNavigation() && animate) {
        const Layout layout = buildLayout();
        if(layout.rowCount >= 3) {
            const int16_t stepDistance = static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y);
            const int magnitude = abs(delta);
            if(magnitude > 0 && stepDistance > 0) {
                const float direction = delta > 0 ? 1.0f : -1.0f;
                app.menuRollOffset += direction * static_cast<float>(stepDistance * magnitude);
            }
        }
    }
    app.redraw = true;
    markPersistentStateDirty();
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
        app.lastHomeAdjustAt = millis();
        app.targetDirty = true;
        app.targetCommitAt = millis() + remote_config::kTargetCommitDelayMs;
        app.redraw = true;
        markPersistentStateDirty();
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
        applyLocalMenuPreview(item, app.draftValue);
        app.redraw = true;
        return;
    }

    if(app.editing && usesChoiceEditor(item)) {
        if(delta != 0) {
            app.draftValue = delta > 0 ? 1 : 0;
        }
        app.redraw = true;
        return;
    }

    moveSelection(delta);
}

void triggerRunAction() {
    if(!isRunActionAvailable()) {
        setToast("Controller offline", 1200);
        return;
    }

    const bool nextRunning = !isRunningState();
    const bool gbwMode = isGbwState();
    const char* expectedState = nextRunning ? (gbwMode ? "GRINDING_GBW" : "GRINDING")
                                            : ((gbwMode || app.status.state == "GRINDING_GBW") ? "IDLE_GBW"
                                                                                               : (app.menuValues[2][SETBUTTONPURGE] != 0 &&
                                                                                                  app.status.state == "GRINDING"
                                                                                                      ? "PURGING"
                                                                                                      : "IDLE"));
    bool sentNow = false;
    const bool queued = queuePendingAction(nextRunning ? PendingActionKind::Start : PendingActionKind::Stop,
                                           PendingActionReflection::State,
                                           nullptr,
                                           expectedState,
                                           0,
                                           &sentNow);
    if(!queued) {
        setToast(nextRunning ? "Start not sent" : "Stop not sent");
        return;
    }

    if(sentNow) {
        app.hasStatus = true;
        app.status.state = expectedState;
        app.status.setRpm = app.manualRpm;
        app.status.setWeightMg = app.weightMg;
        if(nextRunning) {
            if(gbwMode) {
                app.status.currentWeightMg = app.weightMg;
            } else {
                app.status.currentRpm = app.manualRpm;
            }
        } else {
            app.status.currentRpm = 0;
        }
        app.lastStatusAt = millis();
        app.lastStatusPollAt = 0;
        app.redraw = true;
        setToast(nextRunning ? "Start sent" : "Stop sent");
        return;
    }

    setToast(nextRunning ? "Start pending" : "Stop pending");
}

void handlePrimaryShortPress() {
    if(useRoundNavigation()) {
        if(app.screen == Screen::Home && !app.editing) {
            triggerRunAction();
        } else {
            activateSelectedItem();
        }
        return;
    }

    if(app.screen == Screen::Home) {
        setScreen(Screen::Main);
        return;
    }

    activateSelectedItem();
}

void toggleModeAction() {
    if(!stateAllowsModeToggle()) {
        setToast("Stop first", 900);
        return;
    }

    const bool nextGbw = !isGbwState();
    app.preferGbw = nextGbw;
    if(app.hasStatus && (app.status.state == "IDLE" || app.status.state == "IDLE_GBW")) {
        app.status.state = nextGbw ? "IDLE_GBW" : "IDLE";
        app.lastStatusAt = millis();
    }
    app.lastStatusPollAt = 0;
    app.redraw = true;
    (void)queuePendingAction(nextGbw ? PendingActionKind::IdleGbw : PendingActionKind::Idle,
                             PendingActionReflection::State,
                             nullptr,
                             nextGbw ? "IDLE_GBW" : "IDLE",
                             0);
    setToast(nextGbw ? "GBW mode" : "Manual mode");
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
    if(remote_config::kDisableIdleSleepForTesting) {
        return false;
    }
    if(kDisableTransportPerfTest) {
        return false;
    }
    if(app.editing || app.targetDirty || isRunningState()) {
        return false;
    }
    if(!remote_board_support_present(remote_board_feature_map().power.deepSleep)) {
        return false;
    }
    return (millis() - app.lastUserActivityAt) >= remote_config::kIdleSleepAfterMs;
}

void prepareForIdleSleep() {
    applyBacklightLevel(0, true);
    remote_board_apply_indicator({});
    stopWebSocketSession();
    if(remote_wifi_is_active()) {
        remote_wifi_stop();
    }
    stopEspNowSession();
    app.currentTransport = LinkTransport::None;
}

void resumeAfterIdleLightSleep(LinkTransport resumeTransport) {
    applyCurrentBacklightLevel(true);
    switchTransportNow(resumeTransport);
    requestFullControllerRefresh();
    app.lastStatusPollAt = millis();
    app.lastBackgroundMenuPollAt = millis();
    refreshBatteryStatus(true);
    markUserActivity();
    if(wokeFromMotionSource()) {
        returnToHomeOnMotion(false);
    }
    setToast("Awake", remote_config::kWakeToastMs);
}

void enterIdleLightSleep(LinkTransport resumeTransport) {
    const RemoteBoardPins& pins = remote_board_pins();
    bool wakeConfigured = false;
    if(remote_board_wake_supported(RemoteWakeSource::Touch)) {
        wakeConfigured = configureWakePin(pins.touchInt) || wakeConfigured;
    }
    if(remote_board_wake_supported(RemoteWakeSource::PrimaryButton)) {
        wakeConfigured = configureWakePin(pins.primaryButton) || wakeConfigured;
    }
    if(remote_board_wake_supported(RemoteWakeSource::EncoderSwitch)) {
        wakeConfigured = configureWakePin(pins.encoderSwitch) || wakeConfigured;
    }
    if(remote_board_wake_supported(RemoteWakeSource::Motion)) {
        wakeConfigured = configureWakePin(pins.imuInt) || wakeConfigured;
    }
    if(!wakeConfigured) {
        return;
    }

    prepareForIdleSleep();

    esp_sleep_enable_gpio_wakeup();
    Serial.flush();
    esp_light_sleep_start();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    clearWakePin(pins.touchInt);
    clearWakePin(pins.primaryButton);
    clearWakePin(pins.encoderSwitch);
    clearWakePin(pins.imuInt);

    resumeAfterIdleLightSleep(resumeTransport);
}

bool enterIdleDeepSleep(uint64_t wakeMask) {
    if(wakeMask == 0) {
        return false;
    }

    const RemoteBoardPins& pins = remote_board_pins();
    if(pins.touchInt >= 0 &&
       (wakeMask & (uint64_t{1} << static_cast<unsigned>(pins.touchInt))) != 0) {
        pinMode(pins.touchInt, INPUT_PULLUP);
    }
    if(pins.primaryButton >= 0 &&
       (wakeMask & (uint64_t{1} << static_cast<unsigned>(pins.primaryButton))) != 0) {
        pinMode(pins.primaryButton, INPUT_PULLUP);
    }
    if(pins.encoderSwitch >= 0 &&
       (wakeMask & (uint64_t{1} << static_cast<unsigned>(pins.encoderSwitch))) != 0) {
        pinMode(pins.encoderSwitch, INPUT_PULLUP);
    }
    if(pins.imuInt >= 0 &&
       (wakeMask & (uint64_t{1} << static_cast<unsigned>(pins.imuInt))) != 0) {
        pinMode(pins.imuInt, INPUT_PULLUP);
    }

    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    if(esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        return false;
    }

    savePersistentSnapshotNow();
    saveSleepSnapshot();
    prepareForIdleSleep();
    Serial.flush();
    esp_deep_sleep_start();
    return true;
}

void enterIdleSleepIfNeeded() {
    if(!idleSleepAllowed()) {
        return;
    }

    const LinkTransport resumeTransport = app.currentTransport == LinkTransport::None ? initialTransport() : app.currentTransport;
    const uint64_t wakeMask = idleDeepSleepSupported() ? deepSleepWakeMask() : 0;
    if(wakeMask != 0 && enterIdleDeepSleep(wakeMask)) {
        return;
    }

    enterIdleLightSleep(resumeTransport);
}

void handlePrimaryLongPress() {
    if(useRoundNavigation() && app.screen == Screen::Home && !app.editing) {
        setScreen(Screen::Main);
        return;
    }

    if(useRoundNavigation()) {
        if(app.editing) {
            cancelEditing();
        } else {
            setScreen(Screen::Home);
        }
        return;
    }

    if(app.screen == Screen::Home) {
        return;
    }
}

void handleSecondaryShortPress() {
    if(useRoundNavigation()) {
        if(app.editing) {
            cancelEditing();
        } else if(app.screen == Screen::Home) {
            if(isRunningState()) {
                return;
            }
            setScreen(Screen::Main);
        } else if(app.screen == Screen::Drive) {
            setScreen(Screen::Main);
        } else if(app.screen == Screen::EspNowPeers) {
            setScreen(Screen::Network);
        } else if(app.screen != Screen::Home) {
            setScreen(Screen::Home);
        }
        return;
    }
}

void handleSecondaryLongPress() {
    if(useRoundNavigation()) {
        if(app.screen == Screen::Home && !app.editing) {
            toggleModeAction();
        }
        return;
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
    debugRoundInput("touch_down", x, y);

    const Layout layout = buildLayout();
    if(useRoundNavigation() && app.editing) {
        touchTracker.mode = TouchMode::EditorDial;
        touchTracker.circularPreferred = true;
        touchTracker.lastAngle = pointAngle(x, y, layout.editorCx, layout.editorCy);
        return;
    }

    if(app.editing &&
       pointInRing(x,
                   y,
                   layout.editorCx,
                   layout.editorCy,
                   layout.editorInnerRadius,
                   static_cast<int16_t>(layout.editorOuterRadius + 24))) {
        touchTracker.mode = TouchMode::EditorDial;
        touchTracker.circularPreferred = true;
        touchTracker.lastAngle = pointAngle(x, y, layout.editorCx, layout.editorCy);
        return;
    }

    if(app.screen == Screen::Home &&
       !useRoundNavigation() &&
       homeModeTouchRect(layout).contains(x, y)) {
        touchTracker.mode = TouchMode::Tap;
        return;
    }

    if(app.screen == Screen::Home &&
       !useRoundNavigation() &&
       homeActionTouchRect(layout).contains(x, y)) {
        touchTracker.mode = TouchMode::Tap;
        return;
    }

    const int16_t homeDialTouchInnerRadius =
        useRoundNavigation()
            ? static_cast<int16_t>(layout.homeDialInnerRadius + kRoundHomeDialTouchInnerInset)
            : layout.homeDialInnerRadius;
    const int16_t homeDialTouchOuterRadius =
        static_cast<int16_t>(layout.homeDialOuterRadius + (useRoundNavigation() ? kRoundHomeDialTouchOuterPad : 22));
    if(app.screen == Screen::Home &&
       pointInRing(x,
                   y,
                   layout.homeDialCx,
                   layout.homeDialCy,
                   homeDialTouchInnerRadius,
                   homeDialTouchOuterRadius)) {
        touchTracker.mode = TouchMode::HomeDial;
        touchTracker.circularPreferred = true;
        touchTracker.lastAngle = pointAngle(x, y, layout.homeDialCx, layout.homeDialCy);
        return;
    }

    if(app.screen == Screen::Home && useRoundNavigation()) {
        touchTracker.mode = TouchMode::HomeDial;
        touchTracker.circularPreferred = true;
        touchTracker.lastAngle = pointAngle(x, y, layout.homeDialCx, layout.homeDialCy);
        return;
    }

    if(app.screen != Screen::Home && !app.editing) {
        if(useRoundNavigation()) {
            touchTracker.mode = TouchMode::MenuScroll;
            touchTracker.scrollAccum = 0;
            debugRoundInput("touch_mode_round_scroll", x, y);
            return;
        }
        touchTracker.mode = TouchMode::MenuScroll;
        debugRoundInput("touch_mode_scroll", x, y);
        return;
    }

    touchTracker.mode = TouchMode::Tap;
}

void updateDialTouch(int16_t x, int16_t y, bool editorMode) {
    if(useRoundNavigation()) {
        const Layout layout = buildLayout();
        const int16_t centerX = editorMode ? layout.editorCx : layout.homeDialCx;
        const int16_t centerY = editorMode ? layout.editorCy : layout.homeDialCy;
        const float prevX = static_cast<float>(touchTracker.lastX - centerX);
        const float prevY = static_cast<float>(touchTracker.lastY - centerY);
        const float currX = static_cast<float>(x - centerX);
        const float currY = static_cast<float>(y - centerY);
        const float motionX = static_cast<float>(x - touchTracker.lastX);
        const float motionY = static_cast<float>(y - touchTracker.lastY);
        const float sampleX = (prevX + currX) * 0.5f;
        const float sampleY = (prevY + currY) * 0.5f;
        constexpr float kRoundDialCenterDeadZone = 10.0f;
        float basisX = sampleX;
        float basisY = sampleY;
        float basisRadius = sqrtf(basisX * basisX + basisY * basisY);
        if(basisRadius < kRoundDialCenterDeadZone) {
            basisX = prevX;
            basisY = prevY;
            basisRadius = sqrtf(basisX * basisX + basisY * basisY);
        }
        if(basisRadius < kRoundDialCenterDeadZone) {
            basisX = currX;
            basisY = currY;
            basisRadius = sqrtf(basisX * basisX + basisY * basisY);
        }

        float contribution = motionX - motionY;
        if(basisRadius >= kRoundDialCenterDeadZone) {
            // Positive tangential motion means clockwise: top->right, right->down,
            // bottom->left, left->up.
            const float tangentX = -basisY / basisRadius;
            const float tangentY = basisX / basisRadius;
            contribution = (motionX * tangentX) + (motionY * tangentY);
        }

        touchTracker.angleAccum += contribution;
        int steps = 0;
        while(touchTracker.angleAccum >= static_cast<float>(kRoundHomeDialSwipeThreshold)) {
            ++steps;
            touchTracker.angleAccum -= static_cast<float>(kRoundHomeDialSwipeThreshold);
        }
        while(touchTracker.angleAccum <= -static_cast<float>(kRoundHomeDialSwipeThreshold)) {
            --steps;
            touchTracker.angleAccum += static_cast<float>(kRoundHomeDialSwipeThreshold);
        }

        if(steps != 0) {
            touchTracker.dragUsed = true;
            playUiStepTick(static_cast<uint8_t>(abs(steps)));
            handleDialDelta(steps);
        }
        return;
    }

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
        playUiStepTick(static_cast<uint8_t>(abs(steps)));
        handleDialDelta(steps);
    }
}

void updateMenuScrollTouch(int16_t x, int16_t y) {
    const Layout layout = buildLayout();
    if(useRoundNavigation()) {
        const int32_t totalDx = static_cast<int32_t>(x) - touchTracker.startX;
        const int32_t totalDy = static_cast<int32_t>(y) - touchTracker.startY;
        if(isRoundTopLevelTabScreen(app.screen) &&
           abs(totalDx) >= kRoundMenuTabSwipeThreshold &&
           abs(totalDx) > abs(totalDy) + kRoundMenuTabSwipeAxisSlack) {
            touchTracker.dragUsed = true;
            const int delta = totalDx > 0 ? 1 : -1;
            const Screen target = cycleRoundMenuTabScreen(app.screen, delta);
            if(target != app.screen) {
                setScreen(target);
            }
            touchTracker = {};
            return;
        }
    }

    const int16_t threshold = useRoundNavigation() ? roundMenuScrollThreshold(layout)
                                                   : static_cast<int16_t>(layout.rowH / 2);
    touchTracker.scrollAccum += static_cast<int16_t>(y - touchTracker.lastY);
    touchTracker.lastY = y;

    if(useRoundNavigation() && useWaveshareRoundUi()) {
        const int16_t stepDistance = layout.rowCount >= 3
                                         ? abs(static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y))
                                         : threshold;
        const int16_t maxOffset = max<int16_t>(stepDistance, static_cast<int16_t>((stepDistance * 9) / 4));
        app.menuRollOffset = clampValue(touchTracker.scrollAccum, -maxOffset, maxOffset);
        if(abs(touchTracker.scrollAccum) >= static_cast<int16_t>(threshold / 3)) {
            touchTracker.dragUsed = true;
        }
        const int previewSteps = stepDistance > 0
                                     ? -lroundf(app.menuRollOffset / static_cast<float>(stepDistance))
                                     : 0;
        syncTouchScrollPreviewAudio(static_cast<int16_t>(previewSteps));
        app.redraw = true;
        return;
    }

    if(useRoundNavigation()) {
        const int16_t stepDistance = layout.rowCount >= 3
                                         ? abs(static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y))
                                         : threshold;
        const int16_t maxOffset = max<int16_t>(threshold, static_cast<int16_t>(stepDistance / 2));
        app.menuRollOffset = clampValue(touchTracker.scrollAccum, -maxOffset, maxOffset);
        if(abs(touchTracker.scrollAccum) >= static_cast<int16_t>(threshold / 2)) {
            touchTracker.dragUsed = true;
        }
        app.redraw = true;
    }

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
        playUiStepTick(static_cast<uint8_t>(abs(steps)));
        debugRoundInput("touch_scroll", y, steps, touchTracker.scrollAccum, threshold);
        moveSelection(steps, !useRoundNavigation());
        if(useRoundNavigation()) {
            const Layout refreshedLayout = buildLayout();
            const int16_t stepDistance = refreshedLayout.rowCount >= 3
                                             ? abs(static_cast<int16_t>(refreshedLayout.menuRows[1].y - refreshedLayout.menuRows[0].y))
                                             : threshold;
            const int16_t maxOffset = max<int16_t>(threshold, static_cast<int16_t>(stepDistance / 2));
            app.menuRollOffset = clampValue(touchTracker.scrollAccum, -maxOffset, maxOffset);
        }
    }
}

bool updateHomeEdgeSwipeTouch(int16_t x, int16_t y) {
    const int32_t dx = static_cast<int32_t>(x) - touchTracker.startX;
    const int32_t dy = static_cast<int32_t>(y) - touchTracker.startY;
    const int16_t threshold = useRoundNavigation() ? kRoundHomeSwipeThreshold
                                                   : static_cast<int16_t>(isLargeCanvas() ? 20 : 16);
    const int16_t axisSlack = useRoundNavigation() ? kRoundHomeSwipeAxisSlack : 6;
    if(abs(dy) >= threshold && abs(dy) > abs(dx) + axisSlack) {
        touchTracker.dragUsed = true;
        setScreen(Screen::Main);
        touchTracker = {};
        return true;
    }
    return false;
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

bool handleRoundNavTouch(int16_t x, int16_t y) {
    if(!isRoundTopLevelTabScreen(app.screen)) {
        return false;
    }
    const Layout layout = buildLayout();
    if(layout.navPrev.contains(x, y)) {
        setScreen(roundNavMenuTarget(app.screen, -1));
        return true;
    }
    if(layout.navNext.contains(x, y)) {
        setScreen(roundNavMenuTarget(app.screen, 1));
        return true;
    }
    return false;
}

void handleHomeTouch(int16_t x, int16_t y) {
    const Layout layout = buildLayout();
    if(useRoundNavigation()) {
        (void)x;
        (void)y;
        return;
    }

    if(!useRoundNavigation() && homeModeTouchRect(layout).contains(x, y)) {
        setScreen(Screen::Main);
        return;
    }

    if(!useRoundNavigation() && homeActionTouchRect(layout).contains(x, y)) {
        toggleModeAction();
    }
}

void handleMenuTouch(int16_t x, int16_t y) {
    const Layout layout = buildLayout();

    if(app.editing) {
        if(!useRoundNavigation() && layout.editCancel.contains(x, y)) {
            cancelEditing();
            return;
        }
        if(!useRoundNavigation() && layout.editSave.contains(x, y)) {
            activateSelectedItem();
            return;
        }
        size_t count = 0;
        const SettingItem* editItems = itemsForScreen(app.screen, &count);
        if(editItems != nullptr && count > 0) {
            const SettingItem& item = editItems[selectionFor(app.screen)];
            if(usesChoiceEditor(item) && useRoundNavigation()) {
                Rect offRect = {};
                Rect onRect = {};
                buildToggleChoiceRects(layout, &offRect, &onRect);
                if(offRect.contains(x, y)) {
                    app.draftValue = 0;
                    activateSelectedItem();
                    return;
                }
                if(onRect.contains(x, y)) {
                    app.draftValue = 1;
                    activateSelectedItem();
                    return;
                }
            }
            if(useRoundNavigation() &&
               pointInRing(x,
                           y,
                           layout.editorCx,
                           layout.editorCy,
                           0,
                           static_cast<int16_t>(layout.editorOuterRadius + 28))) {
                activateSelectedItem();
                return;
            }
        }
        return;
    }

    size_t count = 0;
    const SettingItem* items = itemsForScreen(app.screen, &count);
    if(items == nullptr || count == 0) {
        return;
    }

    if(useRoundNavigation()) {
        Rect prevRect = {};
        Rect heroRect = {};
        Rect nextRect = {};
        buildRoundMenuTouchRects(layout, &prevRect, &heroRect, &nextRect);
        debugRoundInput("menu_tap", x, y,
                        prevRect.contains(x, y) ? 1 : 0,
                        heroRect.contains(x, y) ? 2 : (nextRect.contains(x, y) ? 3 : 0));
        if(prevRect.contains(x, y)) {
            moveSelection(-1);
            return;
        }
        if(heroRect.contains(x, y)) {
            activateSelectedItem();
            return;
        }
        if(nextRect.contains(x, y)) {
            moveSelection(1);
            return;
        }
        return;
    }

    for(uint8_t row = 0; row < layout.rowCount; ++row) {
        const int idx = layout.firstRow + row;
        if(idx >= static_cast<int>(count)) {
            break;
        }
        if(layout.menuRows[row].contains(x, y)) {
            selectionFor(app.screen) = static_cast<uint8_t>(idx);
            app.redraw = true;
            activateSelectedItem();
            return;
        }
    }
}

bool isDuplicateTouchTap(int16_t x, int16_t y) {
    if(app.lastTouchTapAt == 0) {
        return false;
    }

    const uint32_t now = millis();
    if((now - app.lastTouchTapAt) > kTouchDuplicateTapSuppressMs) {
        return false;
    }

    const int32_t dx = static_cast<int32_t>(x) - app.lastTouchTapX;
    const int32_t dy = static_cast<int32_t>(y) - app.lastTouchTapY;
    return (dx * dx) + (dy * dy) <=
           static_cast<int32_t>(kTouchDuplicateTapRadius) * kTouchDuplicateTapRadius;
}

void rememberTouchTap(int16_t x, int16_t y) {
    app.lastTouchTapAt = millis();
    app.lastTouchTapX = x;
    app.lastTouchTapY = y;
}

void handleTouchTap(int16_t x, int16_t y) {
    if(useRoundNavigation() && app.screen != Screen::Home) {
        return;
    }

    if(isDuplicateTouchTap(x, y)) {
        return;
    }
    rememberTouchTap(x, y);

    if(useRoundNavigation()) {
        if(app.screen != Screen::Home && !app.editing) {
            if(handleRoundNavTouch(x, y)) {
                return;
            }
        }
    } else {
        const Layout layout = buildLayout();
        handleTabsTouch(x, y);
        if(y >= layout.tabs[0].y) {
            return;
        }
    }

    if(app.screen == Screen::Home) {
        handleHomeTouch(x, y);
    } else {
        handleMenuTouch(x, y);
    }
}

void handleTouchInput(const RemoteTouchState& touch) {
    if(touch.justPressed) {
        debugRoundInput("touch_press", touch.x, touch.y);
        if(!useRoundNavigation() && app.screen == Screen::Home) {
            const Layout layout = buildLayout();
            if(homeModeTouchRect(layout).contains(touch.x, touch.y) ||
               homeActionTouchRect(layout).contains(touch.x, touch.y)) {
                handleHomeTouch(touch.x, touch.y);
                touchTracker = {};
                return;
            }
        }
        beginTouchTracking(touch.x, touch.y);
        return;
    }

    if(touch.pressed && touchTracker.active) {
        const int32_t dx = static_cast<int32_t>(touch.x) - touchTracker.startX;
        const int32_t dy = static_cast<int32_t>(touch.y) - touchTracker.startY;
        if((dx * dx) + (dy * dy) >= static_cast<int32_t>(kTapMoveThreshold) * kTapMoveThreshold) {
            if(touchTracker.mode == TouchMode::HomeDial || touchTracker.mode == TouchMode::EditorDial) {
                touchTracker.dragUsed = true;
            }
        }

        switch(touchTracker.mode) {
            case TouchMode::HomeDial:
                updateDialTouch(touch.x, touch.y, false);
                break;
            case TouchMode::EditorDial:
                updateDialTouch(touch.x, touch.y, true);
                break;
            case TouchMode::MenuScroll:
                updateMenuScrollTouch(touch.x, touch.y);
                break;
            case TouchMode::HomeEdgeSwipe:
                if(updateHomeEdgeSwipeTouch(touch.x, touch.y)) {
                    return;
                }
                break;
            case TouchMode::Tap:
                debugRoundInput("touch_move", touch.x, touch.y, dx, dy);
            default:
                break;
        }

        if(!touchTracker.active) {
            return;
        }
        touchTracker.lastX = touch.x;
        touchTracker.lastY = touch.y;
        return;
    }

    if(touch.justReleased && touchTracker.active) {
        const uint32_t heldMs = millis() - touchTracker.startedAt;
        debugRoundInput("touch_release", touchTracker.startX, touchTracker.startY, heldMs, touchTracker.dragUsed ? 1 : 0);
        if(useRoundNavigation()) {
            if(useWaveshareRoundUi() && touchTracker.mode == TouchMode::MenuScroll) {
                const Layout layout = buildLayout();
                const int16_t stepDistance = layout.rowCount >= 3
                                                 ? abs(static_cast<int16_t>(layout.menuRows[1].y - layout.menuRows[0].y))
                                                 : 0;
                if(stepDistance > 0) {
                    const int committedSteps = -lroundf(app.menuRollOffset / static_cast<float>(stepDistance));
                    if(committedSteps != 0) {
                        moveSelection(committedSteps, false);
                        app.menuRollOffset += static_cast<float>(committedSteps * stepDistance);
                        app.redraw = true;
                    }
                }
            }
            touchTracker = {};
            return;
        }
        if(!touchTracker.dragUsed) {
            if(heldMs <= remote_config::kTapMaxDurationMs) {
                handleTouchTap(touchTracker.startX, touchTracker.startY);
            }
        }
        touchTracker = {};
    }
}

void appendJsonQuoted(String& out, const String& value) {
    out += '"';
    for(size_t i = 0; i < value.length(); ++i) {
        const char ch = value.charAt(i);
        switch(ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    out += '"';
}

void appendJsonQuoted(String& out, const char* value) {
    appendJsonQuoted(out, value != nullptr ? String(value) : String(""));
}

bool parseInt32Token(const String& token, int32_t* outValue) {
    if(outValue == nullptr || token.length() == 0) {
        return false;
    }

    char* endPtr = nullptr;
    const long parsed = strtol(token.c_str(), &endPtr, 10);
    if(endPtr == nullptr || *endPtr != '\0') {
        return false;
    }

    *outValue = static_cast<int32_t>(parsed);
    return true;
}

String takeToken(String* text) {
    if(text == nullptr) {
        return "";
    }

    text->trim();
    if(text->length() == 0) {
        return "";
    }

    const int split = text->indexOf(' ');
    if(split < 0) {
        const String token = *text;
        *text = "";
        return token;
    }

    const String token = text->substring(0, split);
    *text = text->substring(split + 1);
    text->trim();
    return token;
}

uint8_t menuValueCount(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return NUM_MENU1_ITEMS;
        case 2: return NUM_MENU2_ITEMS;
        case 3: return NUM_MENU3_ITEMS;
        case 4: return NUM_MENU4_ITEMS;
        default: return 0;
    }
}

const SettingItem* findSettingItem(uint8_t menuNum, uint8_t itemIdx) {
    const SettingItem* groups[] = {kMainItems, kPurgeItems, kGbwItems, kNetworkItems};
    const size_t groupCounts[] = {arrayCount(kMainItems), arrayCount(kPurgeItems), arrayCount(kGbwItems), arrayCount(kNetworkItems)};
    for(size_t group = 0; group < (sizeof(groups) / sizeof(groups[0])); ++group) {
        for(size_t idx = 0; idx < groupCounts[group]; ++idx) {
            const SettingItem& item = groups[group][idx];
            if(item.menuNum == menuNum && item.itemIdx == itemIdx) {
                return &item;
            }
        }
    }
    return nullptr;
}

void appendRemoteControllerStatusJson(String& out) {
    if(!app.hasStatus) {
        out += "null";
        return;
    }

    out += '{';
    out += "\"state\":";
    appendJsonQuoted(out, app.status.state);
    out += ",\"drive\":";
    appendJsonQuoted(out, app.status.drive);
    out += ",\"comm\":";
    appendJsonQuoted(out, app.status.comm);
    out += ",\"motor\":";
    appendJsonQuoted(out, app.status.motor);
    out += ",\"scale\":";
    appendJsonQuoted(out, app.status.scale);
    out += ",\"error\":";
    out += String(app.status.error);
    out += ",\"setRPM\":";
    out += String(app.status.setRpm);
    out += ",\"motorSetRPM\":";
    out += String(app.status.motorSetRpm);
    out += ",\"currentRPM\":";
    out += String(app.status.currentRpm);
    out += ",\"torquePercent\":";
    out += String(app.status.torquePercent);
    out += ",\"setWeightMg\":";
    out += String(app.status.setWeightMg);
    out += ",\"currentWeightMg\":";
    out += String(app.status.currentWeightMg);
    out += ",\"currentMenu\":";
    out += String(app.status.currentMenu);
    out += ",\"enterMenu\":";
    out += app.status.enterMenu ? "true" : "false";
    out += ",\"jmcBaud\":";
    out += String(app.status.jmcBaud);
    out += ",\"preferredJmcBaud\":";
    out += String(app.status.preferredJmcBaud);
    out += '}';
}

void appendDiscoveredPeersJson(String& out) {
    expireDiscoveredEspNowPeers();
    out += '[';
    bool first = true;
    for(uint8_t i = 0; i < kEspNowPeerSlots; ++i) {
        const DiscoveredEspNowPeer& peer = app.espNowPeers[i];
        if(!peer.valid) {
            continue;
        }
        if(!first) {
            out += ',';
        }
        first = false;
        out += '{';
        out += "\"name\":";
        appendJsonQuoted(out, espNowPeerName(peer));
        out += ",\"target\":";
        appendJsonQuoted(out, peer.target);
        out += ",\"mac\":";
        appendJsonQuoted(out, formatMacAddress(peer.mac));
        out += ",\"channel\":";
        out += String(peer.channel);
        out += ",\"locked\":";
        out += lockedEspNowPeerMatches(peer.mac) ? "true" : "false";
        out += ",\"seenAt\":";
        out += String(peer.seenAt);
        out += '}';
    }
    out += ']';
}

void appendRemoteStatusJson(String& out) {
    out += '{';
    out += "\"board\":";
    appendJsonQuoted(out, remote_board_info().name);
    out += ",\"screen\":";
    appendJsonQuoted(out, screenTitle(app.screen));
    out += ",\"editing\":";
    out += app.editing ? "true" : "false";
    out += ",\"selection\":";
    out += String(selectionFor(app.screen));
    out += ",\"transport\":";
    appendJsonQuoted(out, transportName(app.currentTransport));
    out += ",\"pendingTransport\":";
    appendJsonQuoted(out, transportName(app.pendingTransport));
    out += ",\"linkOnline\":";
    out += isLinkOnline() ? "true" : "false";
    out += ",\"wsConnected\":";
    out += app.wsConnected ? "true" : "false";
    out += ",\"espNowReady\":";
    out += app.espNowReady ? "true" : "false";
    out += ",\"radioMode\":";
    appendJsonQuoted(out, formatValueLabel(kNetworkItems[0], app.menuValues[1][RADIOMODE]));
    out += ",\"radioModeKnown\":";
    out += app.radioModeKnown ? "true" : "false";
    out += ",\"invertScroll\":";
    appendJsonQuoted(out, app.localInvertScroll != 0 ? "COUNTER_CLOCKWISE" : "CLOCKWISE");
    out += ",\"uiSounds\":";
    out += app.localUiSounds != 0 ? "true" : "false";
    out += ",\"driveSelection\":";
    appendJsonQuoted(out, localDriveSelectionLabel(currentLocalDriveSelection()));
    out += ",\"manualRPM\":";
    out += String(app.manualRpm);
    out += ",\"weightMg\":";
    out += String(app.weightMg);
    out += ",\"preferGbw\":";
    out += app.preferGbw ? "true" : "false";
    out += ",\"toast\":";
    appendJsonQuoted(out, app.toast);
    out += ",\"lockedPeer\":";
    if(hasActiveEspNowPeer()) {
        appendJsonQuoted(out, formatMacAddress(activeEspNowPeerMac()));
    } else {
        out += "null";
    }
    out += ",\"lockedPeerConfigured\":";
    out += app.espNowPeerConfigured ? "true" : "false";
    out += ",\"wifi\":{";
    out += "\"active\":";
    out += remote_wifi_is_active() ? "true" : "false";
    out += ",\"staConnected\":";
    out += remote_wifi_sta_connected() ? "true" : "false";
    out += ",\"staSaved\":";
    out += remote_wifi_has_saved_network() ? "true" : "false";
    out += ",\"staSSID\":";
    appendJsonQuoted(out, remote_wifi_sta_ssid());
    out += ",\"staIP\":";
    appendJsonQuoted(out, remote_wifi_sta_ip());
    out += ",\"apSSID\":";
    appendJsonQuoted(out, remote_wifi_ap_ssid());
    out += ",\"apIP\":";
    appendJsonQuoted(out, remote_wifi_ap_ip());
    out += '}';
    out += ",\"controller\":";
    appendRemoteControllerStatusJson(out);
    out += ",\"peers\":";
    appendDiscoveredPeersJson(out);
    out += '}';
}

void appendRemoteSnapshotJson(String& out) {
    out += '{';
    out += "\"status\":";
    appendRemoteStatusJson(out);
    out += ",\"menus\":[";
    for(uint8_t menuNum = 1; menuNum <= 4; ++menuNum) {
        if(menuNum > 1) {
            out += ',';
        }
        out += '{';
        out += "\"menu\":";
        out += String(menuNum);
        out += ",\"synced\":";
        out += app.menuSynced[menuNum] ? "true" : "false";
        out += ",\"values\":[";
        const uint8_t count = menuValueCount(menuNum);
        for(uint8_t idx = 0; idx < count; ++idx) {
            if(idx > 0) {
                out += ',';
            }
            out += String(app.menuValues[menuNum][idx]);
        }
        out += "]}";
    }
    out += "]}";
}

void printRemoteConsoleAck(const String& cmd, bool ok, const String& message) {
    String out;
    out.reserve(256);
    out += "{\"type\":\"ack\",\"cmd\":";
    appendJsonQuoted(out, cmd);
    out += ",\"ok\":";
    out += ok ? "true" : "false";
    out += ",\"message\":";
    appendJsonQuoted(out, message);
    out += '}';
    Serial.println(out);
}

void printRemoteConsoleStatus() {
    String out;
    out.reserve(2048);
    out += "{\"type\":\"status\",\"data\":";
    appendRemoteStatusJson(out);
    out += '}';
    Serial.println(out);
}

void printRemoteConsoleSnapshot() {
    String out;
    out.reserve(4096);
    out += "{\"type\":\"snapshot\",\"data\":";
    appendRemoteSnapshotJson(out);
    out += '}';
    Serial.println(out);
}

void printRemoteConsolePeers() {
    String out;
    out.reserve(2048);
    out += "{\"type\":\"peers\",\"items\":";
    appendDiscoveredPeersJson(out);
    out += '}';
    Serial.println(out);
}

void printRemoteConsoleHelp() {
    Serial.println("[remote-diag] commands: ?=status, h=help, s=snapshot, v=toggle stream, f=toggle frame trace, p=peer list");
    Serial.println("[remote-diag] api: api status | api snapshot | api set <menu> <item> <value> | api setrpm <rpm> | api setweight <mg>");
    Serial.println("[remote-diag] api: api action <start|stop|idle|idle_gbw|redetect|calibrate|drive auto|drive jmc|drive rt|jmc_baud <baud>|reset_network|reset_defaults confirm|peer_scan|peer_clear|ws_scan|audio_probe|audio_click|audio_swipe|audio_sweep>");
    Serial.println("[remote-diag] transport: transport <ws|espnow|auto>");
    Serial.println("[remote-diag] peers: peer scan | peer list | peer lock <MAC> | peer clear");
    Serial.println("[remote-diag] raw: raw <protocol-frame>");
}

bool remoteConsoleSetTarget(int32_t value, bool weightMode, String* message) {
    const int32_t normalizedValue = normalizeTargetValue(weightMode, value);
    if(weightMode) {
        app.weightMg = normalizedValue;
    } else {
        app.manualRpm = normalizedValue;
    }
    PendingTargetWrite* pending = stagePendingTargetWrite(weightMode, normalizedValue);
    const bool ok = trySendPendingTargetWrite(pending, true) || pending != nullptr;
    app.targetDirty = false;
    app.lastStatusPollAt = 0;
    markPersistentStateDirty();
    if(message != nullptr) {
        *message = ok ? "queued" : "send failed";
    }
    return ok;
}

bool remoteConsoleSetMenuValue(uint8_t menuNum, uint8_t itemIdx, int32_t value, String* message) {
    const SettingItem* item = findSettingItem(menuNum, itemIdx);
    if(item == nullptr) {
        if(message != nullptr) {
            *message = "unknown menu item";
        }
        return false;
    }

    if(item->kind == ItemKind::Action || item->kind == ItemKind::Danger) {
        if(message != nullptr) {
            *message = "item is action-only";
        }
        return false;
    }

    const bool ok = applyMenuValue(*item, clampValue(value, item->minValue, item->maxValue));
    if(message != nullptr) {
        *message = ok ? "queued" : "send failed";
    }
    return ok;
}

bool remoteConsoleRunAction(String commandLine, String* message) {
    String action = takeToken(&commandLine);
    action.toLowerCase();

    if(action == "start" || action == "stop" || action == "idle" || action == "idle_gbw" || action == "calibrate") {
        PendingActionKind kind = PendingActionKind::None;
        const char* expectedState = nullptr;
        PendingActionReflection reflection = PendingActionReflection::State;
        if(action == "start") {
            kind = PendingActionKind::Start;
            expectedState = isGbwState() ? "GRINDING_GBW" : "GRINDING";
        } else if(action == "stop") {
            kind = PendingActionKind::Stop;
            expectedState = (app.hasStatus && app.status.state == "GRINDING" && app.menuValues[2][SETBUTTONPURGE] != 0) ? "PURGING"
                                                                                                                           : (isGbwState() ? "IDLE_GBW" : "IDLE");
        } else if(action == "idle") {
            kind = PendingActionKind::Idle;
            expectedState = "IDLE";
        } else if(action == "idle_gbw") {
            kind = PendingActionKind::IdleGbw;
            expectedState = "IDLE_GBW";
        } else if(action == "calibrate") {
            kind = PendingActionKind::Calibrate;
            expectedState = "CALIBRATING";
        }
        const bool ok = queuePendingAction(kind, reflection, nullptr, expectedState, 0);
        if(message != nullptr) {
            *message = ok ? "queued" : "send failed";
        }
        return ok;
    }

    if(action == "redetect") {
        setLocalDriveSelection(LOCAL_DRIVE_AUTO);
        markPersistentStateDirty();
        const bool ok = queuePendingAction(PendingActionKind::DriveAuto,
                                           PendingActionReflection::None,
                                           "auto",
                                           nullptr,
                                           0);
        if(message != nullptr) {
            *message = ok ? "queued" : "send failed";
        }
        return ok;
    }

    if(action == "drive") {
        String arg0 = takeToken(&commandLine);
        arg0.toLowerCase();
        if(arg0 == "auto") {
            setLocalDriveSelection(LOCAL_DRIVE_AUTO);
            markPersistentStateDirty();
            const bool ok = queuePendingAction(PendingActionKind::DriveAuto,
                                               PendingActionReflection::None,
                                               "auto",
                                               nullptr,
                                               0);
            if(message != nullptr) {
                *message = ok ? "queued" : "send failed";
            }
            return ok;
        }
        if(arg0 == "jmc" || arg0 == "rt") {
            setLocalDriveSelection(arg0 == "jmc" ? LOCAL_DRIVE_JMC : LOCAL_DRIVE_RT);
            markPersistentStateDirty();
            const bool ok = queuePendingAction(arg0 == "jmc" ? PendingActionKind::DriveJmc : PendingActionKind::DriveRt,
                                               PendingActionReflection::Drive,
                                               arg0.c_str(),
                                               arg0 == "jmc" ? "JMC" : "RT",
                                               0);
            if(message != nullptr) {
                *message = ok ? "queued" : "send failed";
            }
            return ok;
        }
        if(message != nullptr) {
            *message = "usage: drive auto|jmc|rt";
        }
        return false;
    }

    if(action == "jmc_baud") {
        const String baud = takeToken(&commandLine);
        if(baud.length() == 0) {
            if(message != nullptr) {
                *message = "missing baud";
            }
            return false;
        }
        const bool ok = queuePendingAction(PendingActionKind::JmcBaud,
                                           PendingActionReflection::JmcBaud,
                                           baud.c_str(),
                                           nullptr,
                                           baud.toInt());
        if(message != nullptr) {
            *message = ok ? "queued" : "send failed";
        }
        return ok;
    }

    if(action == "reset_network") {
        remote_wifi_reset_network_settings();
        clearPollSchedule();
        switchTransportNow(initialTransport());
        requestFullControllerRefresh();
        markPersistentStateDirty();
        if(message != nullptr) {
            *message = "remote network reset";
        }
        return true;
    }

    if(action == "reset_defaults") {
        String confirm = takeToken(&commandLine);
        const bool ok = queuePendingAction(PendingActionKind::ResetDefaults,
                                           PendingActionReflection::None,
                                           confirm.length() > 0 ? confirm.c_str() : nullptr,
                                           nullptr,
                                           0);
        if(message != nullptr) {
            *message = ok ? "queued" : "send failed";
        }
        return ok;
    }

    if(action == "peer_scan") {
        switchTransportNow(LinkTransport::EspNow);
        requestEspNowDiscovery(true);
        if(message != nullptr) {
            *message = "scanning";
        }
        return true;
    }

    if(action == "peer_clear") {
        clearLockedEspNowPeer();
        markPersistentStateDirty();
        if(message != nullptr) {
            *message = "cleared";
        }
        return true;
    }

    if(action == "ws_scan") {
        switchTransportNow(LinkTransport::WebSocket);
        if(!remote_wifi_has_saved_network()) {
            if(message != nullptr) {
                *message = "wifi not set";
            }
            return false;
        }
        requestWebSocketDiscovery(true);
        if(message != nullptr) {
            *message = "scanning";
        }
        return true;
    }

    if(action == "stream") {
        String mode = takeToken(&commandLine);
        mode.toLowerCase();
        g_consoleStreamEnabled = (mode == "on");
        if(message != nullptr) {
            *message = g_consoleStreamEnabled ? "on" : "off";
        }
        return true;
    }

    if(action == "trace") {
        String mode = takeToken(&commandLine);
        mode.toLowerCase();
        g_consoleFrameTraceEnabled = (mode == "on");
        if(message != nullptr) {
            *message = g_consoleFrameTraceEnabled ? "on" : "off";
        }
        return true;
    }

    if(action == "audio_probe") {
        remote_audio_init();
        const RemoteAudioStatus status = remote_audio_status();
        if(message != nullptr) {
            *message = status.codecDetected ? "codec detected" : remote_audio_detail();
        }
        return status.codecDetected;
    }

    if(action == "audio_click" || action == "audio_swipe" || action == "audio_sweep") {
        remote_audio_init();
        RemoteAudioTone tone = RemoteAudioTone::Click;
        if(action == "audio_swipe") {
            tone = RemoteAudioTone::SwipeRight;
        } else if(action == "audio_sweep") {
            tone = RemoteAudioTone::Sweep;
        }
        const bool ok = remote_audio_play(tone);
        if(message != nullptr) {
            *message = ok ? "tone attempted" : remote_audio_detail();
        }
        return ok;
    }

    if(message != nullptr) {
        *message = "unknown action";
    }
    return false;
}

void remoteConsoleTraceFrame(const char* direction, const String& frame) {
    if(!g_consoleFrameTraceEnabled) {
        return;
    }

    String out;
    out.reserve(frame.length() + 96);
    out += "{\"type\":\"frame\",\"dir\":";
    appendJsonQuoted(out, direction);
    out += ",\"transport\":";
    appendJsonQuoted(out, transportName(app.currentTransport));
    out += ",\"frame\":";
    appendJsonQuoted(out, frame);
    out += '}';
    Serial.println(out);
}

void remoteConsoleMaybeStream() {
    if(!g_consoleStreamEnabled || (millis() - g_consoleLastStreamAt) < 1000) {
        return;
    }

    g_consoleLastStreamAt = millis();
    printRemoteConsoleStatus();
}

void handleRemoteConsoleLine(const char* rawLine) {
    String line = rawLine != nullptr ? String(rawLine) : String("");
    line.trim();
    if(line.length() == 0) {
        return;
    }

    if(line == "?" || line.equalsIgnoreCase("status") || line.equalsIgnoreCase("api status")) {
        printRemoteConsoleStatus();
        return;
    }
    if(line == "h" || line.equalsIgnoreCase("help")) {
        printRemoteConsoleHelp();
        return;
    }
    if(line == "s" || line.equalsIgnoreCase("snapshot") || line.equalsIgnoreCase("api snapshot")) {
        printRemoteConsoleSnapshot();
        return;
    }
    if(line == "p") {
        printRemoteConsolePeers();
        return;
    }
    if(line == "v") {
        g_consoleStreamEnabled = !g_consoleStreamEnabled;
        printRemoteConsoleAck("stream", true, g_consoleStreamEnabled ? "on" : "off");
        return;
    }
    if(line == "f") {
        g_consoleFrameTraceEnabled = !g_consoleFrameTraceEnabled;
        printRemoteConsoleAck("trace", true, g_consoleFrameTraceEnabled ? "on" : "off");
        return;
    }

    String tail = line;
    String cmd = takeToken(&tail);
    cmd.toLowerCase();

    if(cmd == "raw") {
        const bool ok = tail.length() > 0 && sendRaw(tail);
        printRemoteConsoleAck(line, ok, ok ? "sent" : "send failed");
        return;
    }

    if(cmd == "transport") {
        tail.toLowerCase();
        bool ok = true;
        String message = "ok";
        if(tail == "ws" || tail == "websocket") {
            app.transportOverrideActive = true;
            switchTransportNow(LinkTransport::WebSocket);
        } else if(tail == "espnow" || tail == "esp-now") {
            app.transportOverrideActive = true;
            switchTransportNow(LinkTransport::EspNow);
            requestEspNowDiscovery(true);
        } else if(tail == "auto") {
            app.transportOverrideActive = false;
            switchTransportNow(initialTransport());
        } else {
            ok = false;
            message = "usage: transport ws|espnow|auto";
        }
        printRemoteConsoleAck(line, ok, message);
        return;
    }

    if(cmd == "peer") {
        String sub = takeToken(&tail);
        sub.toLowerCase();
        if(sub == "scan") {
            switchTransportNow(LinkTransport::EspNow);
            requestEspNowDiscovery(true);
            printRemoteConsoleAck(line, true, "scanning");
            return;
        }
        if(sub == "list") {
            printRemoteConsolePeers();
            return;
        }
        if(sub == "clear") {
            clearLockedEspNowPeer();
            markPersistentStateDirty();
            printRemoteConsoleAck(line, true, "cleared");
            return;
        }
        if(sub == "lock") {
            uint8_t mac[6] = {};
            if(parseMacAddress(tail, mac)) {
                setLockedEspNowPeer(mac);
                if(app.espNowReady) {
                    ensureEspNowPeer(mac);
                }
                switchTransportNow(LinkTransport::EspNow);
                markPersistentStateDirty();
                printRemoteConsoleAck(line, true, "locked");
            } else {
                printRemoteConsoleAck(line, false, "usage: peer lock <MAC>");
            }
            return;
        }
        printRemoteConsoleAck(line, false, "usage: peer scan|list|lock <MAC>|clear");
        return;
    }

    if(cmd == "api") {
        String sub = takeToken(&tail);
        sub.toLowerCase();
        if(sub == "set") {
            int32_t menuNum = 0;
            int32_t itemIdx = 0;
            int32_t value = 0;
            const String menuToken = takeToken(&tail);
            const String itemToken = takeToken(&tail);
            const String valueToken = takeToken(&tail);
            const bool ok = parseInt32Token(menuToken, &menuNum) &&
                            parseInt32Token(itemToken, &itemIdx) &&
                            parseInt32Token(valueToken, &value);
            String message;
            bool applied = false;
            if(ok) {
                applied = remoteConsoleSetMenuValue(static_cast<uint8_t>(menuNum),
                                                    static_cast<uint8_t>(itemIdx),
                                                    value,
                                                    &message);
            } else {
                message = "usage: api set <menu> <item> <value>";
            }
            printRemoteConsoleAck(line, applied, message);
            return;
        }
        if(sub == "setrpm") {
            int32_t value = 0;
            const bool ok = parseInt32Token(takeToken(&tail), &value);
            String message;
            const bool applied = ok ? remoteConsoleSetTarget(value, false, &message) : false;
            printRemoteConsoleAck(line, applied, ok ? message : String("usage: api setrpm <rpm>"));
            return;
        }
        if(sub == "setweight") {
            int32_t value = 0;
            const bool ok = parseInt32Token(takeToken(&tail), &value);
            String message;
            const bool applied = ok ? remoteConsoleSetTarget(value, true, &message) : false;
            printRemoteConsoleAck(line, applied, ok ? message : String("usage: api setweight <mg>"));
            return;
        }
        if(sub == "action") {
            String message;
            const bool ok = remoteConsoleRunAction(tail, &message);
            printRemoteConsoleAck(line, ok, message);
            return;
        }
        printRemoteConsoleAck(line, false, "usage: api status|snapshot|set|setrpm|setweight|action");
        return;
    }

    printRemoteConsoleAck(line, false, "unknown command");
}

void remoteConsolePoll() {
    while(Serial.available() > 0) {
        const int raw = Serial.read();
        if(raw < 0) {
            break;
        }

        const char ch = static_cast<char>(raw);
        if(ch == '\r' || ch == '\n') {
            if(g_consoleLineLen == 0) {
                continue;
            }
            g_consoleLineBuffer[g_consoleLineLen] = '\0';
            handleRemoteConsoleLine(g_consoleLineBuffer);
            g_consoleLineLen = 0;
            continue;
        }

        if(g_consoleLineLen + 1 < sizeof(g_consoleLineBuffer)) {
            g_consoleLineBuffer[g_consoleLineLen++] = ch;
        }
    }
}

}  // namespace

void remote_app_setup() {
    Serial.begin(115200);
    remote_runtime_bind_current_task();
    delay(100);
    remote_board_console().println("SG98 multi-board remote starting");

    if(!remote_board_init()) {
        setToast("Board init failed", 60000);
    }
    if(!kDisableTransportPerfTest) {
        remote_wifi_init();
        wsClient.onEvent(onWsEvent);
    } else {
        stopAllTransportStacks();
    }
    g_statePrefsReady = g_statePrefs.begin(kPersistentStateNamespace, false);
    seedMenuDefaults();
    app.manualRpm = default_setRPM;
    app.weightMg = default_setWeight;
    app.preferGbw = false;
    app.redraw = true;
    app.lastUserActivityAt = millis();
    app.lastControlActivityAt = app.lastUserActivityAt;
    app.currentTransport = LinkTransport::None;
    app.lastTransportSwitchAt = millis();
    LinkTransport startupTransport = initialTransport();
    restorePersistentSnapshot(&startupTransport);
    restoreSleepSnapshot(&startupTransport);
    refreshBatteryStatus(true);
    applyCurrentBacklightLevel(true);

    if(kDisableTransportPerfTest) {
        app.currentTransport = LinkTransport::None;
        app.pendingTransport = LinkTransport::None;
        app.pendingTransportAt = 0;
        clearPollSchedule();
        setToast("UI debug mode", 3000);
    } else {
        switchTransportNow(startupTransport);
        requestFullControllerRefresh();
    }

    printRemoteConsoleHelp();
    printRemoteConsoleStatus();
}

void remote_app_loop() {
    RemoteInputState input = {};
    const uint32_t now = millis();
    remote_board_poll_input(&input);
    remote_audio_service();
    remoteConsolePoll();

    const bool hasControlInput =
        input.dialDelta != 0 || input.primaryPressed || input.primaryLongPress || input.primaryShortPress ||
        input.secondaryPressed || input.secondaryLongPress || input.secondaryShortPress ||
        input.touch.pressed || input.touch.justPressed || input.touch.justReleased;
    if(hasControlInput) {
        app.lastControlActivityAt = now;
        app.motionHomeCandidateAt = 0;
    }

    if(input.dialDelta != 0 || input.motionDetected || input.primaryPressed ||
       input.primaryLongPress || input.primaryShortPress ||
       input.secondaryPressed || input.secondaryLongPress || input.secondaryShortPress ||
       input.touch.pressed || input.touch.justPressed || input.touch.justReleased) {
        markUserActivity();
    }

    if(input.motionDetected && handleMotionHomeGesture(now)) {
        input.dialDelta = 0;
        input.primaryPressed = false;
        input.primaryShortPress = false;
        input.primaryLongPress = false;
        input.secondaryPressed = false;
        input.secondaryShortPress = false;
        input.secondaryLongPress = false;
        input.touch = {};
    }

    if(input.primaryLongPress) {
        handlePrimaryLongPress();
    }
    if(input.primaryShortPress) {
        handlePrimaryShortPress();
    }
    if(input.secondaryLongPress) {
        handleSecondaryLongPress();
    }
    if(input.secondaryShortPress) {
        handleSecondaryShortPress();
    }

    handleTouchInput(input.touch);
    handleDialDelta(applyDialDirection(input.dialDelta));
    commitTargetIfDue();
    servicePersistentSnapshotSave();
    const bool yieldTransport = shouldYieldTransportToUi();
    if(!kDisableTransportPerfTest && !yieldTransport) {
        const uint32_t transportServiceInterval = currentTransportServiceIntervalMs();
        const uint32_t pollingServiceInterval = currentPollingServiceIntervalMs();
        if(app.transportYieldedToUi) {
            app.lastTransportServiceAt = now;
            app.lastPollingServiceAt = now;
            app.transportYieldedToUi = false;
        }
        if((now - app.lastTransportServiceAt) >= transportServiceInterval) {
            manageTransport();
            servicePendingControllerSync();
            handleSendCompletion();
            updateSyncFromStatusFallback();
            app.lastTransportServiceAt = now;
        }
        if((now - app.lastPollingServiceAt) >= pollingServiceInterval) {
            syncPolling();
            refreshBatteryStatus(false);
            app.lastPollingServiceAt = now;
        }
    } else {
        if(yieldTransport) {
            app.transportYieldedToUi = true;
        }
        refreshBatteryStatus(false);
    }
    remote_board_apply_indicator(currentIndicator());
    updateHomeWheelAnimation();
    updateMenuRollAnimation();
    remoteConsoleMaybeStream();
    enterIdleSleepIfNeeded();

    if(app.redraw) {
        redrawUi();
    }

    remote_runtime_wait_for_input_or_timeout(computeLoopWaitMs(millis(), yieldTransport));
}
