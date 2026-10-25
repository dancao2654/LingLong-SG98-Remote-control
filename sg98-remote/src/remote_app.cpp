#include "remote_app.h"

#include <math.h>
#include <new>
#include <string.h>

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <esp_now.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include "remote_board.h"
#include "remote_audio.h"
#include "remote_config.h"
#include "remote_pressure_ble.h"
#include "remote_runtime.h"
#include "remote_voice.h"
#include "remote_wifi.h"
#include "sg98_remote_defs.h"
#include "sg98_ota_download.h"
#include "sg98_ota_source.h"
#include "sg98_ota_update.h"
#include "sg98_system_ota.h"
#include "sg98_ota_transport.h"
#include "sg98_connection_reveal.h"
#include "assets/sg98_cartoon_pixels.h"
#include "sg98_version.h"
#include <sg98_remote_protocol.h>

namespace {

constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorBg = 0x0841;
constexpr uint16_t kColorPanel = 0x10A3;
constexpr uint16_t kColorPanelHi = 0x18E5;
constexpr uint16_t kColorAccent = 0x2CF9;
constexpr uint16_t kColorAccent2 = 0xFD20;
constexpr uint16_t kColorGauge = kColorAccent2;
constexpr uint16_t kColorWarn = 0xFD20;
constexpr uint16_t kColorDanger = 0xF9C6;
constexpr uint16_t kColorGood = 0x3D8A;
constexpr uint16_t kColorPowerHold = 0x5677;
constexpr uint16_t kColorEspNow = 0x55FF;
constexpr uint16_t kColorMuted = 0x7BEF;
constexpr uint16_t kColorText = 0xFFFF;
constexpr bool kShowBurrGapReadout = false;

constexpr uint16_t kTapMoveThreshold = 10;
constexpr uint32_t kTouchLongPressMs = 650;
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
constexpr bool kLoopLatencyDebug = false;
constexpr uint32_t kLoopSectionWarnUs = 12000;
constexpr uint32_t kLoopTotalWarnUs = 30000;
constexpr uint32_t kTransportUiYieldMs = 280;
constexpr uint32_t kHomeTransportQuietMs = 1000;
constexpr uint32_t kPersistentStateUiQuietMs = 8000;
constexpr int kMenuEncoderStepDivisor = 2;
constexpr uint32_t kStaleEspNowLockClearMs = 2500;
constexpr uint32_t kHomeTransportServiceMs = 450;
constexpr uint32_t kHomePollingServiceMs = 650;
constexpr uint32_t kHomeStatusPollIdleMs = 650;
constexpr uint32_t kHomeStatusPollRunningMs = 1200;
constexpr uint32_t kHomeStatusPollActiveMs = 2400;
constexpr uint32_t kHomeGaugePollingServiceMs = 75;
constexpr uint32_t kHomeGaugeStatusPollMs = 150;
constexpr uint32_t kHomeGaugeFastPollHoldMs = 2500;
constexpr int8_t kBatteryPercentJitter = 1;
constexpr int8_t kBatteryTopOffPercent = 98;
constexpr int32_t kBatteryVoltageJitterMv = 25;
constexpr int32_t kBatteryVbusJitterMv = 150;
constexpr uint32_t kPersistentStateDebounceMs = 1200;
constexpr uint32_t kValidUnixTime = 1700000000UL;
constexpr uint32_t kDockWeatherHttpTimeoutMs = 4500;
constexpr uint32_t kDockWeatherRetryMs = 60000;
constexpr uint32_t kDockWeatherQuickRetryMs = 8000;
constexpr uint32_t kNetworkFetchMinFreeHeap = 30000;
constexpr uint32_t kFirmwareCheckUserQuietMs = 5000;
constexpr uint32_t kAutoOtaFirstCheckDelayMs = sg98_ota_update::kOnlineCheckIntervalMs;
constexpr uint32_t kAutoOtaRetryAfterFailureMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kAutoOtaRetryAfterNoSavedWifiMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kControllerOtaCheckBusyMs = 75000UL;
constexpr uint32_t kControllerOtaDownloadBusyMs = 8UL * 60UL * 1000UL;
constexpr uint32_t kControllerOtaApplyBusyMs = 30000UL;
constexpr uint32_t kControllerOtaRebootFlashMs = 10000UL;
constexpr uint32_t kControllerOtaDownloadConfirmMs = 15000UL;
constexpr uint32_t kControllerOtaProgressRedrawMs = 1000UL;
constexpr uint32_t kControllerEnvironmentRetryMs = 60000;
constexpr uint32_t kEnvironmentShareRetryMs = 30000;
constexpr uint32_t kAutoEspNowReturnFlashMs = 5200;
constexpr uint32_t kWifiReturnCurveHoldMs = 9000;
constexpr uint32_t kWebFallbackEspNowHoldMs = 120000;
constexpr uint32_t kWebFallbackRecoveryHintMs = 1000;
constexpr uint32_t kWebFallbackWifiReleaseDelayMs = 5000;
constexpr uint8_t kWifiFallbackMinStaAttempts = 3;
constexpr bool kMotionHomeEnabled = false;
constexpr uint32_t kMotionHomeConfirmWindowMs = 1500;
constexpr uint32_t kMotionHomeMinGapMs = 200;
constexpr uint32_t kMotionHomeCooldownMs = 2200;
constexpr uint32_t kMotionHomeIgnoreAfterControlMs = 900;
constexpr const char* kControllerApHost = "192.168.4.1";
constexpr uint8_t kWebSocketSubnetProbeBatch = 1;
constexpr uint16_t kWebSocketSubnetProbeTimeoutMs = 55;
constexpr uint32_t kWebSocketSubnetProbeIntervalMs = 150;
constexpr uint32_t kWebSocketCandidateRetryMs = 6500;
constexpr const char* kEspNowRecoveryHintCommand = "RECOVER_ESPNOW";
constexpr char kPersistentStateNamespace[] = "remote-app";
constexpr char kPersistentStateKey[] = "snapshot";

#ifndef REMOTE_VOICE_ALLOW_START
#define REMOTE_VOICE_ALLOW_START 0
#endif

enum class Screen : uint8_t {
    Home = 0,
    Main = 1,
    Purge = 2,
    Gbw = 3,
    AudioDiag = 4,
    Drive = 5,
    EspNowPeers = 6,
    Network = 7,
    Profiler = 8,
    About = 9,
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

enum class NetworkBadgeMode : uint8_t {
    Off,
    EspNow,
    WifiJoin,
    WifiAp,
    WifiLan,
};

constexpr uint8_t kScreenCount = 10;
constexpr uint8_t kControllerMenuMax = 5;
constexpr uint8_t kControllerMenuSlots = kControllerMenuMax + 1;
constexpr uint8_t kEspNowPeerSlots = 6;
constexpr uint8_t kPendingMenuWriteSlots = 8;
constexpr uint16_t kPressureProfileSampleCount =
    static_cast<uint16_t>(remote_config::kPressureProfilerWindowMs /
                          remote_config::kPressureProfilerSampleMs) +
    1;
constexpr uint32_t kEspNowDiscoveryIntervalMs = 1200;
constexpr uint32_t kEspNowDiscoveryExpireMs = 5000;
constexpr uint32_t kWifiRecoveryWindowMs = 32000;
constexpr uint32_t kAutoRadioRequestMs = 2500;
constexpr uint32_t kEspNowSendConfirmTimeoutMs = 450;
constexpr uint32_t kEspNowSendRetryBackoffMs = 80;
constexpr uint8_t kEspNowReliableSendAttempts = 6;
constexpr uint8_t kEspNowBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kEspNowMinChannel = 1;
constexpr uint8_t kEspNowMaxChannel = 13;
constexpr int32_t kControllerRadioWeb = 0;
constexpr int32_t kControllerRadioEspNow = 1;
constexpr int32_t kControllerRadioOff = 2;
constexpr int32_t kControllerRadioDual = 3;

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

struct InputDiagnostics {
    uint32_t lastEventAt = 0;
    uint32_t primaryShortCount = 0;
    uint32_t primaryLongCount = 0;
    uint32_t secondaryShortCount = 0;
    uint32_t secondaryLongCount = 0;
    uint32_t touchPressCount = 0;
    uint32_t touchReleaseCount = 0;
    uint32_t motionCount = 0;
    uint32_t displayRotationCount = 0;
    uint32_t powerChangeCount = 0;
    int16_t touchX = 0;
    int16_t touchY = 0;
    uint8_t displayRotation = 0;
    bool primaryPressed = false;
    bool secondaryPressed = false;
    bool touchPressed = false;
    char lastEvent[24] = "boot";
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
    DriveKrs,
    DriveXb130,
    DriveSew,
    DriveTse,
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

struct PressureProfilerState {
    bool adcReady = false;
    bool armed = false;
    bool running = false;
    bool hasSample = false;
    int8_t adcPin = -1;
    uint32_t armedAt = 0;
    uint32_t startedAt = 0;
    uint32_t lastSampleAt = 0;
    uint32_t lastRedrawAt = 0;
    uint32_t lastAboveSenseAt = 0;
    uint32_t senseCandidateAt = 0;
    uint16_t currentCentiBar = 0;
    uint16_t peakCentiBar = 0;
    uint16_t samples[kPressureProfileSampleCount] = {};
    uint32_t sampleTimes[kPressureProfileSampleCount] = {};
    uint16_t sampleHead = 0;
    uint16_t sampleCount = 0;
};

struct DockWeatherState {
    bool valid = false;
    int16_t temperatureF = 0;
    int8_t precipitationPercent = -1;
    uint16_t weatherCode = 0;
    uint32_t updatedAt = 0;
    String location;
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
    int32_t menuValues[kControllerMenuSlots][sg98_protocol::kMaxMenuValues] = {};
    int32_t controllerMenuValues[kControllerMenuSlots][sg98_protocol::kMaxMenuValues] = {};
    bool menuSynced[kControllerMenuSlots] = {};
    bool controllerMenuSynced[kControllerMenuSlots] = {};
    sg98_protocol::StatusFrame status;
    sg98_protocol::StatusFrame controllerStatus;
    bool hasStatus = false;
    bool hasControllerStatus = false;
    bool preferGbw = false;
    bool redraw = true;
    bool deferredHomeStatusRedraw = false;
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
    bool dockClockActive = false;
    bool dockClockTimeConfigured = false;
    bool controllerOtaActionBusy = false;
    bool controllerOtaDownloadArmed = false;
    LinkTransport currentTransport = LinkTransport::None;
    LinkTransport pendingTransport = LinkTransport::None;
    uint32_t targetCommitAt = 0;
    uint32_t pendingTransportAt = 0;
    uint32_t lastTransportSwitchAt = 0;
    uint32_t toastUntil = 0;
    uint32_t flashUntil = 0;
    uint32_t lastLinkActivityAt = 0;
    uint32_t lastAckAt = 0;
    uint32_t lastStatusAt = 0;
    uint32_t lastStatusPollAt = 0;
    uint32_t lastGaugeStatusChangeAt = 0;
    uint32_t lastSafetyHeartbeatAt = 0;
    uint32_t lastBackgroundMenuPollAt = 0;
    uint32_t lastMdnsQueryAt = 0;
    uint32_t wsConnectStartedAt = 0;
    uint32_t lastWifiServiceAt = 0;
    uint32_t lastWebSocketServiceAt = 0;
    uint32_t lastTransportServiceAt = 0;
    uint32_t lastPollingServiceAt = 0;
    uint32_t lastRoundInputDebugAt = 0;
    uint32_t lastMenuPollAt[kControllerMenuSlots] = {};
    uint32_t lastMenuRxAt[kControllerMenuSlots] = {};
    uint32_t lastEspNowDiscoveryAt = 0;
    uint32_t lastAutoRadioRequestAt = 0;
    uint32_t webRecoveryStartedAt = 0;
    uint32_t webFallbackEspNowUntil = 0;
    uint32_t lastWebFallbackRecoveryHintAt = 0;
    uint32_t webFallbackWifiReleaseAt = 0;
    uint8_t espNowChannel = remote_config::kEspNowChannel;
    bool transportYieldedToUi = false;
    bool transportOverrideActive = false;
    bool autoEspNowReturnNotified = false;
    bool preserveWifiForEspNowRecovery = false;
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
    uint32_t dockClockLastRedrawAt = 0;
    uint32_t dockClockLastNtpSyncAt = 0;
    uint32_t dockClockLastWeatherFetchAt = 0;
    uint32_t dockClockLastWeatherOkAt = 0;
    int16_t dockClockLastWeatherHttpCode = 0;
    uint32_t lastControllerEnvironmentRequestAt = 0;
    uint32_t lastEnvironmentShareAt = 0;
    uint32_t nextAutoOtaCheckAt = 0;
    uint32_t controllerOtaActionStartedAt = 0;
    uint32_t controllerOtaActionUntil = 0;
    uint32_t controllerOtaDownloadArmUntil = 0;
    uint32_t lastControllerOtaProgressAt = 0;
    uint32_t homeWheelAnimatedAt = 0;
    uint32_t homeWheelFrameAt = 0;
    int16_t menuEncoderRemainder = 0;
    int16_t lastTouchTapX = 0;
    int16_t lastTouchTapY = 0;
    RemoteBatteryStatus battery;
    DockWeatherState dockWeather;
    sg98_ota_update::State otaUpdate;
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
    uint16_t wsSubnetProbeCursor = 0;
    String toast = "Booting";
    String networkCurveHoldLabel;
    uint32_t networkCurveHoldUntil = 0;
    String flashTitle;
    String flashSubtitle;
    String controllerOtaProgressLabel;
    uint16_t flashAccent = kColorAccent;
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
    bool burrGapFieldsPresent = false;
    bool burrGapEnabled = false;
    bool burrGapKnown = false;
    bool burrGapLive = false;
    int32_t burrGapUm = 0;
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
    int32_t menuValues[kControllerMenuSlots][sg98_protocol::kMaxMenuValues] = {};
    uint8_t menuSyncedMask = 0;
    uint8_t transport = 0;
    bool preferGbw = false;
    bool radioModeKnown = false;
    bool espNowPeerConfigured = false;
    uint8_t espNowPeerMac[6] = {};
    bool hasStatus = false;
    PersistedStatusFrame status = {};
};

struct RemoteOtaWifiGuard {
    LinkTransport previousTransport = LinkTransport::None;
    bool switchedTransport = false;
};

bool systemOtaBusy();
void confirmSystemOta();
void serviceSystemOta();
void serviceSystemRemoteInstall();
void finishSystemOta(bool ok, const String& detail);
bool handleSystemOtaAck(const sg98_protocol::AckFrame& ack);

template <size_t N>
constexpr size_t arrayCount(const SettingItem (&)[N]) {
    return N;
}

bool isInvertScrollItem(const SettingItem& item);
bool isRadioModeItem(const SettingItem& item);
bool isGaugeSetItem(const SettingItem& item);
String gaugeGapLabelFromMenuValue(int32_t value);
bool isConfirmActionItem(const SettingItem& item);
bool isControllerOtaActionItem(const SettingItem& item);
bool controllerOtaDownloadArmed();
bool usesChoiceEditor(const SettingItem& item);
bool hasActiveEspNowPeer();
bool isEspNowPeerSlotItem(const SettingItem& item);
bool isEspNowPeerScanItem(const SettingItem& item);
bool isEspNowPeerClearItem(const SettingItem& item);
bool isControllerLinkItem(const SettingItem& item);
String espNowPeerLabelForItem(const SettingItem& item);
bool pendingControllerSyncActive();
void requestWebSocketDiscovery(bool resetSession);
void switchTransportNow(LinkTransport transport);
void restoreOtaWifi(const RemoteOtaWifiGuard& guard);
bool ensureOtaWifi(RemoteOtaWifiGuard* guard, String* message);
bool setEspNowChannel(uint8_t channel);
bool webFallbackEspNowActive(uint32_t now);
void remoteConsolePoll();
void remoteConsoleMaybeStream();
void remoteConsoleTraceFrame(const char* direction, const String& frame);

static_assert(NUM_MENU1_ITEMS <= sg98_protocol::kMaxMenuValues, "menu1 frame too small");
static_assert(NUM_MENU2_ITEMS <= sg98_protocol::kMaxMenuValues, "menu2 frame too small");
static_assert(NUM_MENU3_ITEMS <= sg98_protocol::kMaxMenuValues, "menu3 frame too small");
static_assert(NUM_MENU4_ITEMS <= sg98_protocol::kMaxMenuValues, "menu4 frame too small");
static_assert(NUM_MENU5_ITEMS <= sg98_protocol::kMaxMenuValues, "menu5 frame too small");

constexpr SettingItem kMainItems[] = {
    {"Sleep time", ItemKind::Value, 1, SETSLEEP, 0, 300, 5, "min", nullptr, nullptr},
    {"Brightness", ItemKind::Value, 1, DISP_BRIGHTNESS, 1, 255, 1, "", nullptr, nullptr},
    {"LED max Brightness", ItemKind::Value, 1, LED_BRIGHTNESS, 0, 100, 1, "%", nullptr, nullptr},
    {"Invert scrolling", ItemKind::Toggle, 1, INVERT_SCROLL, 0, 1, 1, "", nullptr, nullptr},
    {"Audio diag", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "audio"},
    {"Reset default", ItemKind::Danger, 0, 0, 0, 0, 0, "", "reset_defaults", "confirm"},
    {"About", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "about"},
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
    {"Gauge Set", ItemKind::Value, 1, GAUGESET, -1200, 1200, 1, "um", nullptr, nullptr},
    {"Max RPM", ItemKind::Value, 1, SETMAX, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "rpm", nullptr, nullptr},
    {"Min RPM", ItemKind::Value, 1, SETMIN, absolute_min_rpm, 500, rpm_scalar, "rpm", nullptr, nullptr},
    {"Motor torque", ItemKind::Value, 1, SETMOTORTORQUE, 1, 300, 1, "%", nullptr, nullptr},
    {"Motor ramp", ItemKind::Value, 1, SETMOTORRAMP, 0, 12, 1, "lvl", nullptr, nullptr},
    {"Motor direction", ItemKind::Toggle, 1, SETMOTORDIRECTION, 0, 1, 1, "", nullptr, nullptr},
    {"Drive", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "drive"},
    {"Calibrate", ItemKind::Action, 0, 0, 0, 0, 0, "", "calibrate", nullptr},
    {"System Update", ItemKind::Action, 0, 0, 0, 0, 0, "", "system_ota", nullptr},
    {"GbW RPM", ItemKind::Value, 3, GBW_RPM_SET, 200, 1500, rpm_scalar, "rpm", nullptr, nullptr},
    {"GbW Slow RPM", ItemKind::Value, 3, GBW_SLOW_RPM, rpm_scalar, 500, rpm_scalar, "rpm", nullptr, nullptr},
    {"GbW Slow mg", ItemKind::Value, 3, GBW_SLOW_MG, 0, 6000, 100, "mg", nullptr, nullptr},
    {"Start delay", ItemKind::Value, 3, GBW_BUTTON_DELAY, 0, 1000, 50, "ms", nullptr, nullptr},
    {"SpeedModifier", ItemKind::Value, 3, GBW_SPEEDMOD, 1, 32767, 100, "", nullptr, nullptr},
    {"Time offset", ItemKind::Value, 3, GBW_OFFSET, 10, 1000, 10, "ms", nullptr, nullptr},
    {"Save scale", ItemKind::Toggle, 3, SAVE_SCALE, 0, 1, 1, "", nullptr, nullptr},
};

constexpr SettingItem kNetworkItems[] = {
    {"Wireless", ItemKind::Value, 1, RADIOMODE, 0, 2, 1, "", nullptr, nullptr},
    {"Controller link", ItemKind::Action, 0, 0, 0, 0, 0, "", "screen", "peers"},
    {"System Update", ItemKind::Action, 0, 0, 0, 0, 0, "", "system_ota", nullptr},
    {"Reset Network", ItemKind::Action, 0, 0, 0, 0, 0, "", "reset_network", nullptr},
};

constexpr SettingItem kAudioItems[] = {
    {"UI sounds", ItemKind::Toggle, 0, 0, 0, 1, 1, "", "ui_sounds", nullptr},
    {"Voice command", ItemKind::Action, 0, 0, 0, 0, 0, "", "voice", nullptr},
    {"Probe codec", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_probe", nullptr},
    {"Play click", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_click", nullptr},
    {"Play swipe", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_swipe", nullptr},
    {"Play sweep", ItemKind::Action, 0, 0, 0, 0, 0, "", "audio_sweep", nullptr},
};

constexpr SettingItem kDriveItems[] = {
    {"Auto detect", ItemKind::Action, 0, 0, 0, 0, 0, "", "redetect", nullptr},
    {"Force JMC", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "jmc"},
    {"Force RT", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "rt"},
    {"Force KRS", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "krs"},
    {"Force XB130", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "xb130"},
    {"Force SEW", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "sew"},
    {"Force TSE", ItemKind::Action, 0, 0, 0, 0, 0, "", "drive", "tse"},
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
mdns_search_once_t* g_mdnsQuery = nullptr;
bool g_mdnsStopPending = false;
uint32_t g_lastSubnetProbeAt = 0;
uint32_t g_transportGeneration = 0;
uint32_t g_autoOtaWifiStartedAt = 0;
uint32_t g_autoOtaWifiGeneration = 0;
RemoteOtaWifiGuard g_autoOtaWifiGuard;
RemoteInputState g_consoleUiInput;
bool g_consoleUiInputPending = false;
bool g_remoteOtaRequested = false;

struct SystemOtaState {
    sg98_system_ota::Flow flow;
    sg98_system_ota::Stage sentStage = sg98_system_ota::Stage::Idle;
    LinkTransport previousTransport = LinkTransport::None;
    bool previousOverride = false;
    bool remoteInstallRequested = false;
    uint32_t lastProbeAt = 0;
    uint32_t lastProgressAt = 0;
    String detail;
    String expectedVersion;
    String expectedSha;
    String controllerVersion;
    String controllerSha;
    String controllerEnv;
    String controllerUi = "Unknown/legacy";
    bool controllerLocalUi = false;
    bool controllerIdentityProbePending = false;
    bool controllerIdentityLegacy = false;
    bool controllerLocalPush = false;
    bool legacyUpdateSent = false;
    bool restoreTransportPending = false;
    bool remoteBootVerificationPending = false;
    sg98_ota_download::ManifestSpec remoteManifest;
    sg98_ota_download::ManifestSpec controllerManifest;
    uint32_t controllerImageOffset = 0;
    volatile uint8_t downloadPart = 0;
    volatile uint32_t downloadWritten = 0;
    volatile uint32_t downloadTotal = 0;
    uint16_t transferSession = 0;
    uint32_t transferOffset = 0;
    uint32_t transferSentAt = 0;
    uint8_t transferRetries = 0;
    uint8_t transferPhase = 0;
    bool transferAwaitingAck = false;
    String transferSendError;
};
SystemOtaState g_systemOta;
sg98_connection_reveal::State g_connectionReveal;
uint32_t g_connectionRevealLastFrame = 0;
uint16_t* g_connectionRevealBitmap = nullptr;

bool systemOtaBusy() { return g_systemOta.flow.busy(); }

enum class BackgroundNetworkKind : uint8_t { Weather, OtaManifest, SystemOtaStage };

struct BackgroundNetworkRequest {
    BackgroundNetworkKind kind;
    String url;
    String payload;
    int httpCode = -1;
    bool ok = false;
    sg98_ota_download::ManifestSpec manifest;
    sg98_ota_download::ManifestSpec controllerManifest;
    sg98_ota_download::Result result;
    sg98_ota_download::Result controllerResult;
    uint32_t controllerImageOffset = 0;
    volatile uint8_t progressPart = 0;
    volatile uint32_t progressWritten = 0;
    volatile uint32_t progressTotal = 0;
};

sg98_ota_transport::AckFrame g_systemOtaAck = {};
bool g_systemOtaAckPending = false;
portMUX_TYPE g_systemOtaAckMux = portMUX_INITIALIZER_UNLOCKED;

QueueHandle_t g_networkRequests = nullptr;
QueueHandle_t g_networkResults = nullptr;
bool g_networkPending = false;
PressureProfilerState g_pressureProfiler;
TouchTracker touchTracker;
InputDiagnostics g_inputDiagnostics;
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
volatile uint32_t g_espNowTxDelivered = 0;
volatile uint32_t g_espNowTxFailed = 0;
uint32_t g_espNowTxEnqueueFailed = 0;
uint32_t g_consoleLastStreamAt = 0;
char g_consoleLineBuffer[384] = {};
size_t g_consoleLineLen = 0;

constexpr uint32_t kSleepSnapshotMagic = 0x53473938UL;
constexpr uint16_t kSleepSnapshotVersion = 11;
constexpr uint16_t kSleepSnapshotVersionLegacyUiSoundsOff = 6;
constexpr uint32_t kInteractiveLoopWaitMs = 4;
constexpr uint32_t kAnimatingLoopWaitMs = 16;
constexpr uint32_t kIdleLoopWaitCapMs = 80;

enum LocalDriveSelection : int32_t {
    LOCAL_DRIVE_AUTO = 0,
    LOCAL_DRIVE_JMC = 1,
    LOCAL_DRIVE_RT = 2,
    LOCAL_DRIVE_KRS = 3,
    LOCAL_DRIVE_XB130 = 4,
    LOCAL_DRIVE_SEW = 5,
    LOCAL_DRIVE_TSE = 6,
};

bool useRoundNavigation();
bool roundUiInteractionActive();
bool roundUiRecentActivity(uint32_t quietMs);
bool isRunningState();
void servicePressureProfiler(uint32_t now);
void shortenWaitForPressureProfiler(uint32_t now, uint32_t* waitMs);
void armPressureProfiler();
void leavePressureProfiler();
void serviceDockClock(uint32_t now);
void shortenWaitForDockClock(uint32_t now, uint32_t* waitMs);
void exitDockClock();
uint8_t dockClockBacklightLevel();
void applyBacklightLevel(uint8_t level, bool force);
void applyCurrentBacklightLevel(bool force);
void requestControllerEnvironment(bool force);
bool sendRaw(const String& command);
void markUserActivity(uint32_t now = millis());
void showFlashScreen(const String& title, const String& subtitle, uint32_t durationMs, uint16_t accent);
bool setOtaBootPartition(String target, String* message);
bool downloadOtaManifest(String url, String* message);
bool runVoiceCommand(String* message, bool showToast);

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

bool statusHasEnabledGauge(const sg98_protocol::StatusFrame& status) {
    return status.burrGapFieldsPresent && status.burrGapEnabled;
}

bool statusHasLiveGauge(const sg98_protocol::StatusFrame& status) {
    return statusHasEnabledGauge(status) && status.burrGapKnown && status.burrGapLive;
}

bool homeGaugeFastPollActive(uint32_t now = millis()) {
    if(!useRoundNavigation() || app.screen != Screen::Home || !app.hasStatus ||
       !statusHasEnabledGauge(app.status)) {
        return false;
    }

    if(statusHasLiveGauge(app.status)) {
        return true;
    }

    return app.lastGaugeStatusChangeAt != 0 &&
           (now - app.lastGaugeStatusChangeAt) < kHomeGaugeFastPollHoldMs;
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
        if(homeGaugeFastPollActive()) {
            return kHomeGaugePollingServiceMs;
        }
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
    if(app.flashUntil > now) {
        shortenWaitForDeadline(now, app.flashUntil, &waitMs);
    }

    shortenWaitForPressureProfiler(now, &waitMs);
    shortenWaitForDockClock(now, &waitMs);

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

    if(!remote_config::kDisableIdleSleepForTesting && !app.battery.usbPresent) {
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
    return (radioMode == kControllerRadioWeb || radioMode == kControllerRadioDual)
               ? LinkTransport::WebSocket
               : LinkTransport::EspNow;
}

int32_t sanitizeControllerRadioMode(int32_t value) {
    if(value == kControllerRadioWeb || value == kControllerRadioEspNow ||
       value == kControllerRadioOff) {
        return value;
    }
    if(value == kControllerRadioDual) {
        return kControllerRadioWeb;
    }
    return kControllerRadioWeb;
}

int32_t safeUserRadioMode(int32_t value) {
    return sanitizeControllerRadioMode(value);
}

String radioModeValueLabel(int32_t value) {
    switch(sanitizeControllerRadioMode(value)) {
        case kControllerRadioWeb:
            return "WIFI";
        case kControllerRadioEspNow:
            return "ESP-NOW";
        case kControllerRadioOff:
            return "RADIO OFF";
        default:
            return "WIFI";
    }
}

float normalizedRadioModeLevel(int32_t value) {
    switch(sanitizeControllerRadioMode(value)) {
        case kControllerRadioWeb:
            return 1.0f;
        case kControllerRadioEspNow:
            return 0.5f;
        case kControllerRadioOff:
            return 0.0f;
        default:
            return 1.0f;
    }
}

int32_t stepRadioModeValue(int32_t current, int delta) {
    static const int32_t values[] = {
        kControllerRadioWeb,
        kControllerRadioEspNow,
        kControllerRadioOff,
    };
    constexpr int count = sizeof(values) / sizeof(values[0]);
    if(delta == 0) {
        return safeUserRadioMode(current);
    }

    int selected = 0;
    const int32_t sanitized = safeUserRadioMode(current);
    for(int i = 0; i < count; ++i) {
        if(values[i] == sanitized) {
            selected = i;
            break;
        }
    }

    const int direction = delta > 0 ? 1 : -1;
    int steps = abs(delta);
    while(steps-- > 0) {
        selected += direction;
        if(selected < 0) selected = count - 1;
        if(selected >= count) selected = 0;
    }
    return values[selected];
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

bool connectedToControllerAp() {
    const String ssid = remote_wifi_sta_ssid();
    return ssid.startsWith("SG98-") && !ssid.startsWith("SG98-REMOTE-");
}

bool validLanHost(const IPAddress& ip) {
    return ip[0] != 0 && ip[3] != 0 && ip[3] != 255 && ip != WiFi.localIP();
}

IPAddress controllerProbeCandidate(uint16_t cursor) {
    const IPAddress local = WiFi.localIP();
    if(local[0] == 0) {
        return IPAddress();
    }

    int host = -1;
    if(cursor < 128) {
        const int delta = static_cast<int>(cursor / 2) + 1;
        host = (cursor % 2 == 0) ? static_cast<int>(local[3]) - delta
                                 : static_cast<int>(local[3]) + delta;
    } else {
        host = static_cast<int>(((cursor - 128) % 254) + 1);
    }
    if(host <= 0 || host >= 255) {
        return IPAddress();
    }
    return IPAddress(local[0], local[1], local[2], static_cast<uint8_t>(host));
}

bool probeControllerWebSocketHost(const IPAddress& ip) {
    if(!validLanHost(ip)) {
        return false;
    }

    WiFiClient client;
    const bool ok = client.connect(ip, remote_config::kWebSocketPort, kWebSocketSubnetProbeTimeoutMs);
    client.stop();
    return ok;
}

bool probeControllerSubnet(IPAddress* foundIp) {
    if(!remote_wifi_sta_connected()) {
        return false;
    }

    bool attempted = false;
    for(uint8_t attempt = 0; attempt < kWebSocketSubnetProbeBatch; ++attempt) {
        const IPAddress candidate = controllerProbeCandidate(app.wsSubnetProbeCursor++);
        if(!validLanHost(candidate)) {
            continue;
        }
        attempted = true;
        if(probeControllerWebSocketHost(candidate)) {
            if(foundIp != nullptr) {
                *foundIp = candidate;
            }
            app.wsSubnetProbeCursor = 0;
            Serial.printf("[remote_ws] subnet probe found %s:%u\n",
                          candidate.toString().c_str(),
                          static_cast<unsigned>(remote_config::kWebSocketPort));
            return true;
        }
    }

    if(attempted) {
        Serial.printf("[remote_ws] subnet probe cursor=%u\n",
                      static_cast<unsigned>(app.wsSubnetProbeCursor));
    }
    return false;
}

LinkTransport initialTransport() {
    return LinkTransport::EspNow;
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
        case RemoteBoardId::LilyGoT4S3H678:
            return true;
        default:
            return false;
    }
}

int16_t menuTouchScrollDelta(int16_t dy) {
    if(!useRoundNavigation() || !useWaveshareRoundUi()) {
        return dy;
    }

    switch(remote_board_info().id) {
        case RemoteBoardId::WaveshareAmoled175:
            return dy;
        default:
            return static_cast<int16_t>(-dy);
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
    };
    for(RemoteTextStyle style : candidates) {
        if(textFitsWidth(text, width, style, 18)) {
            return style;
        }
    }
    return RemoteTextStyle::Small;
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

String controllerConnectionText() {
    const char* radio = app.currentTransport == LinkTransport::WebSocket ? "WIFI" :
                        app.currentTransport == LinkTransport::EspNow ? "ESP-NOW" : "NET";
    return String(radio) + (isLinkOnline() ? " CTRL ONLINE" : " CTRL OFFLINE");
}

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

bool isNetworkStatusToast(const String& text) {
    return text == "CONNECTING WI-FI" ||
           text == "WIFI MODE" ||
           text == "WEBSOCKET MODE" ||
           text == "ESP-NOW MODE" ||
           text == "SEARCHING CONTROLLER";
}

String networkCurveStatusText() {
    const bool wifiRequested = app.currentTransport == LinkTransport::WebSocket ||
                               app.pendingTransport == LinkTransport::WebSocket ||
                               (app.radioModeKnown &&
                                transportFromRadioMode(app.menuValues[1][RADIOMODE]) == LinkTransport::WebSocket);
    if(remote_wifi_is_active() || wifiRequested) {
        if(remote_wifi_sta_connected()) {
            const String ip = remote_wifi_sta_ip();
            return ip.length() > 0 ? String("WIFI LAN ") + ip : String("WIFI LAN");
        }
        if(remote_wifi_ap_running()) {
            const String ip = remote_wifi_ap_ip();
            return ip.length() > 0 ? String("WIFI AP ") + ip : String("WIFI AP");
        }
        if(remote_wifi_has_saved_network()) {
            const uint8_t attempts = min<uint8_t>(remote_wifi_sta_attempt_count(), kWifiFallbackMinStaAttempts);
            if(attempts > 0) {
                return String("WIFI JOIN ") + String(static_cast<unsigned>(attempts)) + "/" +
                       String(static_cast<unsigned>(kWifiFallbackMinStaAttempts));
            }
            return remote_wifi_is_active() ? String("WIFI JOIN") : String("WIFI START");
        }
        return "WIFI SETUP";
    }

    if(app.currentTransport == LinkTransport::EspNow) {
        if(isLinkOnline()) {
            return "ESP-NOW LINK";
        }
        return hasActiveEspNowPeer() ? "ESP-NOW OFFLINE" : "ESP-NOW SEARCH";
    }

    return "NETWORK OFF";
}

String roundHomeBottomLabel() {
    const uint32_t now = millis();
    if(app.networkCurveHoldUntil > now && app.networkCurveHoldLabel.length() > 0) {
        return upperText(app.networkCurveHoldLabel);
    }
    if(app.networkCurveHoldUntil != 0 && now >= app.networkCurveHoldUntil) {
        app.networkCurveHoldUntil = 0;
        app.networkCurveHoldLabel = "";
    }

    if(app.toastUntil > now) {
        const String toast = upperText(app.toast);
        if(!isNetworkStatusToast(toast)) {
            return toast;
        }
    }
    return networkCurveStatusText();
}

void serviceNetworkStatusRedraw() {
    static String previousLabel;
    const String label = controllerConnectionText() + "|" + networkCurveStatusText() + "|" + roundHomeBottomLabel();
    if(label != previousLabel) {
        previousLabel = label;
        app.redraw = true;
    }
}

void holdNetworkCurveStatus(const String& label, uint32_t durationMs) {
    app.networkCurveHoldLabel = label;
    app.networkCurveHoldUntil = millis() + durationMs;
    app.redraw = true;
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
    if(isRadioModeItem(item)) {
        return "CONTROLLER WI-FI ACCESS";
    }
    if(isGaugeSetItem(item)) {
        return "10 MICRON STEPS";
    }
    if(isConfirmActionItem(item)) {
        return "CHOOSE NO OR YES";
    }
    if(isControllerOtaActionItem(item)) {
        return controllerOtaDownloadArmed() ? "CONFIRM DOWNLOAD" : "CHECK CONTROLLER UPDATE";
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
        case Screen::Profiler: return "PROFILE";
        case Screen::About: return "ABOUT";
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
    if(current == Screen::About) {
        return Screen::Main;
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

String homeGapValueLabel() {
    if(!app.hasStatus || !app.status.burrGapFieldsPresent || !app.status.burrGapEnabled) {
        return "-";
    }
    if(!app.status.burrGapKnown) {
        return "--";
    }
    const int32_t wholeDisplayUm = (app.status.burrGapUm / 10) * 10;
    return String(wholeDisplayUm);
}

String dockGapLine() {
    if(!app.hasStatus || !app.status.burrGapFieldsPresent || !app.status.burrGapEnabled ||
       !app.status.burrGapKnown) {
        return "-- um";
    }
    return homeGapValueLabel() + " um";
}

void drawScaledGlyph(RemoteCanvas& canvas,
                     int16_t x,
                     int16_t y,
                     const uint8_t* rows,
                     uint8_t width,
                     uint8_t height,
                     uint8_t scale,
                     uint16_t color) {
    for(uint8_t row = 0; row < height; ++row) {
        for(uint8_t col = 0; col < width; ++col) {
            if((rows[row] & (1u << (width - 1 - col))) == 0) {
                continue;
            }
            canvas.fillRect(static_cast<int16_t>(x + col * scale),
                            static_cast<int16_t>(y + row * scale),
                            scale,
                            scale,
                            color);
        }
    }
}

void drawMicronUnitIcon(RemoteCanvas& canvas,
                        int16_t x,
                        int16_t centerY,
                        RemoteTextStyle style,
                        uint16_t color) {
    static constexpr uint8_t kMicronRows[] = {
        0b00000,
        0b10010,
        0b10010,
        0b10010,
        0b10010,
        0b11110,
        0b10000,
        0b10000,
    };
    static constexpr uint8_t kMeterRows[] = {
        0b00000,
        0b00000,
        0b11010,
        0b10101,
        0b10101,
        0b10101,
        0b10101,
        0b00000,
    };
    const uint8_t scale = textScale(style);
    const int16_t y = static_cast<int16_t>(centerY - 4 * scale);
    drawScaledGlyph(canvas, x, y, kMicronRows, 5, 8, scale, color);
    drawScaledGlyph(canvas, static_cast<int16_t>(x + 6 * scale), y, kMeterRows, 5, 8, scale, color);
}

void drawGapReadout(RemoteCanvas& canvas,
                    int16_t centerX,
                    int16_t centerY,
                    const String& value,
                    RemoteTextStyle valueStyle,
                    RemoteTextStyle unitStyle,
                    uint16_t color,
                    uint16_t bgColor) {
    const uint8_t valueScale = textScale(valueStyle);
    const uint8_t unitScale = textScale(unitStyle);
    const bool showUnit = value != "-" && value != "--";
    const int16_t unitW = showUnit ? static_cast<int16_t>(11 * unitScale) : 0;
    const int16_t gap = showUnit ? static_cast<int16_t>(max<uint8_t>(4, 2 * valueScale)) : 0;
    const int16_t numberW = estimatedTextWidth(value, valueStyle);
    const int16_t totalW = static_cast<int16_t>(numberW + gap + unitW);
    const int16_t left = static_cast<int16_t>(centerX - totalW / 2);
    canvas.drawCenteredText(value,
                            static_cast<int16_t>(left + numberW / 2),
                            centerY,
                            valueStyle,
                            color,
                            bgColor);
    if(showUnit) {
        drawMicronUnitIcon(canvas,
                           static_cast<int16_t>(left + numberW + gap),
                           centerY,
                           unitStyle,
                           color);
    }
}

void drawGaugeGapMenuValue(RemoteCanvas& canvas,
                           int16_t centerX,
                           int16_t centerY,
                           int32_t menuValue,
                           RemoteTextStyle valueStyle,
                           RemoteTextStyle unitStyle,
                           uint16_t color,
                           uint16_t bgColor) {
    drawGapReadout(canvas,
                   centerX,
                   centerY,
                   gaugeGapLabelFromMenuValue(menuValue),
                   valueStyle,
                   unitStyle,
                   color,
                   bgColor);
}

void drawGaugeGapMenuValueTopRight(RemoteCanvas& canvas,
                                   int16_t rightX,
                                   int16_t topY,
                                   int32_t menuValue,
                                   RemoteTextStyle valueStyle,
                                   RemoteTextStyle unitStyle,
                                   uint16_t color,
                                   uint16_t bgColor) {
    const String value = gaugeGapLabelFromMenuValue(menuValue);
    const uint8_t valueScale = textScale(valueStyle);
    const uint8_t unitScale = textScale(unitStyle);
    const int16_t unitW = static_cast<int16_t>(11 * unitScale);
    const int16_t gap = static_cast<int16_t>(max<uint8_t>(4, 2 * valueScale));
    const int16_t numberW = estimatedTextWidth(value, valueStyle);
    const int16_t totalW = static_cast<int16_t>(numberW + gap + unitW);
    const int16_t left = static_cast<int16_t>(rightX - totalW);
    const int16_t centerY = static_cast<int16_t>(topY + 4 * max<uint8_t>(valueScale, unitScale));

    canvas.drawCenteredText(value,
                            static_cast<int16_t>(left + numberW / 2),
                            centerY,
                            valueStyle,
                            color,
                            bgColor);
    drawMicronUnitIcon(canvas,
                       static_cast<int16_t>(left + numberW + gap),
                       centerY,
                       unitStyle,
                       color);
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

int32_t batteryAbsDiff(int32_t lhs, int32_t rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

bool batteryValueClose(int32_t lhs, int32_t rhs, int32_t tolerance) {
    if(lhs < 0 || rhs < 0) {
        return lhs == rhs;
    }
    return batteryAbsDiff(lhs, rhs) <= tolerance;
}

bool batteryTopOffUsbHold(const RemoteBatteryStatus& battery) {
    return battery.usbPresent &&
           battery.batteryPresent &&
           battery.percent >= kBatteryTopOffPercent;
}

bool batteryStatusEqual(const RemoteBatteryStatus& lhs, const RemoteBatteryStatus& rhs) {
    const bool topOffEquivalent = batteryTopOffUsbHold(lhs) && batteryTopOffUsbHold(rhs);
    const bool chargeStateEqual =
        topOffEquivalent ||
        (lhs.charging == rhs.charging &&
         lhs.discharging == rhs.discharging &&
         lhs.standby == rhs.standby);

    return lhs.supported == rhs.supported &&
           lhs.batteryPresent == rhs.batteryPresent &&
           lhs.usbPresent == rhs.usbPresent &&
           chargeStateEqual &&
           batteryValueClose(lhs.percent, rhs.percent, kBatteryPercentJitter) &&
           batteryValueClose(lhs.millivolts, rhs.millivolts, kBatteryVoltageJitterMv) &&
           batteryValueClose(lhs.vbusMillivolts, rhs.vbusMillivolts, kBatteryVbusJitterMv) &&
           batteryValueClose(lhs.systemMillivolts, rhs.systemMillivolts, kBatteryVoltageJitterMv);
}

bool batteryStatusVisualEqual(const RemoteBatteryStatus& lhs, const RemoteBatteryStatus& rhs) {
    const bool topOffEquivalent = batteryTopOffUsbHold(lhs) && batteryTopOffUsbHold(rhs);
    const bool chargeStateEqual =
        topOffEquivalent ||
        (lhs.charging == rhs.charging &&
         lhs.discharging == rhs.discharging &&
         lhs.standby == rhs.standby);

    return lhs.supported == rhs.supported &&
           lhs.batteryPresent == rhs.batteryPresent &&
           lhs.usbPresent == rhs.usbPresent &&
           chargeStateEqual &&
           batteryValueClose(lhs.percent, rhs.percent, kBatteryPercentJitter);
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

String pressureBarLabel(uint16_t centiBar) {
    const uint16_t roundedDeciBar = static_cast<uint16_t>((centiBar + 5) / 10);
    String out;
    out.reserve(8);
    out += String(static_cast<int>(roundedDeciBar / 10));
    out += '.';
    out += String(static_cast<int>(roundedDeciBar % 10));
    return out;
}

String pressureElapsedLabel(uint32_t elapsedMs) {
    const uint32_t seconds = (elapsedMs + 500) / 1000;
    String out;
    out.reserve(8);
    out += String(static_cast<unsigned long>(seconds));
    out += 's';
    return out;
}

int8_t pressureProfilerAdcPin() {
    if(remote_config::kPressureSensorAdcPin >= 0) {
        return remote_config::kPressureSensorAdcPin;
    }

    const RemoteBoardPins& pins = remote_board_pins();
    if(pins.expansionGpio0 >= 0) {
        return pins.expansionGpio0;
    }
    if(pins.batterySense >= 0) {
        return pins.batterySense;
    }
    return -1;
}

void configurePressureProfilerAdc() {
    if(g_pressureProfiler.adcReady) {
        return;
    }

    g_pressureProfiler.adcPin = pressureProfilerAdcPin();
    if(g_pressureProfiler.adcPin < 0) {
        return;
    }

    pinMode(g_pressureProfiler.adcPin, INPUT);
#if defined(ARDUINO_ARCH_ESP32)
    analogReadResolution(12);
    analogSetPinAttenuation(g_pressureProfiler.adcPin, ADC_11db);
#endif
    g_pressureProfiler.adcReady = true;
}

uint16_t pressureCentiBarFromMillivolts(uint32_t millivolts) {
    const uint16_t zeroMv = remote_config::kPressureSensorZeroMillivolts;
    const uint16_t fullMv = remote_config::kPressureSensorFullScaleMillivolts;
    if(millivolts <= zeroMv || fullMv <= zeroMv) {
        return 0;
    }

    const uint32_t spanMv = fullMv - zeroMv;
    const uint32_t scaled =
        ((millivolts - zeroMv) * remote_config::kPressureSensorFullScaleCentiBar + (spanMv / 2)) / spanMv;
    return static_cast<uint16_t>(min<uint32_t>(scaled, remote_config::kPressureSensorFullScaleCentiBar));
}

uint16_t readPressureCentiBar() {
    configurePressureProfilerAdc();
    if(!g_pressureProfiler.adcReady || g_pressureProfiler.adcPin < 0) {
        return 0;
    }

    uint32_t totalMv = 0;
    constexpr uint8_t kReadCount = 4;
    for(uint8_t i = 0; i < kReadCount; ++i) {
        totalMv += static_cast<uint32_t>(analogReadMilliVolts(g_pressureProfiler.adcPin));
    }
    return pressureCentiBarFromMillivolts((totalMv + (kReadCount / 2)) / kReadCount);
}

void clearPressureProfilerSamples() {
    g_pressureProfiler.sampleHead = 0;
    g_pressureProfiler.sampleCount = 0;
    g_pressureProfiler.peakCentiBar = 0;
    g_pressureProfiler.hasSample = false;
}

void appendPressureProfilerSample(uint16_t centiBar, uint32_t elapsedMs) {
    g_pressureProfiler.samples[g_pressureProfiler.sampleHead] = centiBar;
    g_pressureProfiler.sampleTimes[g_pressureProfiler.sampleHead] = elapsedMs;
    g_pressureProfiler.sampleHead =
        static_cast<uint16_t>((g_pressureProfiler.sampleHead + 1) % kPressureProfileSampleCount);
    if(g_pressureProfiler.sampleCount < kPressureProfileSampleCount) {
        ++g_pressureProfiler.sampleCount;
    }
    g_pressureProfiler.peakCentiBar = max<uint16_t>(g_pressureProfiler.peakCentiBar, centiBar);
    g_pressureProfiler.hasSample = true;
}

bool pressureProfilerSampleAt(uint16_t ageIndex, uint16_t* centiBar, uint32_t* elapsedMs) {
    if(ageIndex >= g_pressureProfiler.sampleCount) {
        return false;
    }

    const uint16_t head = g_pressureProfiler.sampleHead;
    const uint16_t index =
        static_cast<uint16_t>((head + kPressureProfileSampleCount - 1u - ageIndex) %
                              kPressureProfileSampleCount);
    if(centiBar != nullptr) {
        *centiBar = g_pressureProfiler.samples[index];
    }
    if(elapsedMs != nullptr) {
        *elapsedMs = g_pressureProfiler.sampleTimes[index];
    }
    return true;
}

void armPressureProfiler() {
    configurePressureProfilerAdc();
    remote_pressure_ble_begin();
    g_pressureProfiler.armed = true;
    g_pressureProfiler.running = false;
    g_pressureProfiler.armedAt = millis();
    g_pressureProfiler.startedAt = 0;
    g_pressureProfiler.lastSampleAt = 0;
    g_pressureProfiler.lastRedrawAt = 0;
    g_pressureProfiler.lastAboveSenseAt = 0;
    g_pressureProfiler.senseCandidateAt = 0;
    g_pressureProfiler.currentCentiBar = readPressureCentiBar();
    clearPressureProfilerSamples();
    setToast(g_pressureProfiler.adcReady ? "Profiler armed" : "Pressure ADC?", 1200);
}

void leavePressureProfiler() {
    g_pressureProfiler.armed = false;
    g_pressureProfiler.running = false;
    remote_pressure_ble_end();
}

void resetPressureProfilerRun(uint32_t now) {
    g_pressureProfiler.running = false;
    g_pressureProfiler.startedAt = 0;
    g_pressureProfiler.lastSampleAt = now;
    g_pressureProfiler.lastAboveSenseAt = 0;
    g_pressureProfiler.senseCandidateAt = 0;
    clearPressureProfilerSamples();
}

void startPressureProfilerRun(uint32_t now, uint16_t centiBar) {
    g_pressureProfiler.running = true;
    g_pressureProfiler.startedAt = now;
    g_pressureProfiler.lastAboveSenseAt = now;
    g_pressureProfiler.senseCandidateAt = 0;
    clearPressureProfilerSamples();
    appendPressureProfilerSample(centiBar, 0);
    setToast("Profile started", 900);
}

void servicePressureProfiler(uint32_t now) {
    if(app.screen != Screen::Profiler) {
        return;
    }

    remote_pressure_ble_begin();
    if((now - g_pressureProfiler.lastSampleAt) < remote_config::kPressureProfilerSampleMs) {
        remote_pressure_ble_service(g_pressureProfiler.currentCentiBar, app.battery.percent, now);
        return;
    }

    const uint16_t centiBar = readPressureCentiBar();
    g_pressureProfiler.currentCentiBar = centiBar;
    g_pressureProfiler.lastSampleAt = now;
    remote_pressure_ble_service(centiBar, app.battery.percent, now);

    if(app.screen != Screen::Profiler) {
        return;
    }

    if(!g_pressureProfiler.armed) {
        armPressureProfiler();
    }

    if(!g_pressureProfiler.running) {
        if(centiBar >= remote_config::kPressureProfilerSenseCentiBar) {
            if(g_pressureProfiler.senseCandidateAt == 0) {
                g_pressureProfiler.senseCandidateAt = now;
            } else if((now - g_pressureProfiler.senseCandidateAt) >= remote_config::kPressureProfilerSenseHoldMs) {
                startPressureProfilerRun(now, centiBar);
            }
        } else {
            g_pressureProfiler.senseCandidateAt = 0;
        }

        if((now - g_pressureProfiler.lastRedrawAt) >= remote_config::kPressureProfilerRedrawMs) {
            g_pressureProfiler.lastRedrawAt = now;
            app.redraw = true;
        }
        return;
    }

    const uint32_t elapsed = now - g_pressureProfiler.startedAt;
    appendPressureProfilerSample(centiBar, elapsed);
    if(centiBar >= remote_config::kPressureProfilerSenseCentiBar) {
        g_pressureProfiler.lastAboveSenseAt = now;
    }
    if((now - g_pressureProfiler.lastRedrawAt) >= remote_config::kPressureProfilerRedrawMs) {
        g_pressureProfiler.lastRedrawAt = now;
        app.redraw = true;
    }
}

void shortenWaitForPressureProfiler(uint32_t now, uint32_t* waitMs) {
    if(waitMs == nullptr) {
        return;
    }
    if(app.screen != Screen::Profiler) {
        return;
    }
    shortenWaitForDeadline(now, g_pressureProfiler.lastSampleAt + remote_config::kPressureProfilerSampleMs, waitMs);
    if(app.screen == Screen::Profiler) {
        shortenWaitForDeadline(now, g_pressureProfiler.lastRedrawAt + remote_config::kPressureProfilerRedrawMs, waitMs);
    }
}

bool isLinkOnline() {
    return app.lastLinkActivityAt != 0 &&
           (millis() - app.lastLinkActivityAt) < remote_config::kLinkTimeoutMs;
}

bool isEspNowLinkOnline() {
    return isLinkOnline() && app.currentTransport == LinkTransport::EspNow;
}

uint16_t linkStatusColor() {
    if(!isLinkOnline()) {
        return kColorDanger;
    }
    return isEspNowLinkOnline() ? kColorEspNow : kColorGood;
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

bool statusStateIsRunning(const String& state) {
    return state == "GRINDING" || state == "GRINDING_GBW" || state == "PURGING";
}

bool statusStateIsStopped(const String& state) {
    return state == "IDLE" || state == "IDLE_GBW";
}

void ensureDockClockTimezone() {
    static bool timezoneApplied = false;
    if(timezoneApplied) {
        return;
    }
    setenv("TZ", remote_config::kDockClockTimezone, 1);
    tzset();
    timezoneApplied = true;
}

bool localClockTime(struct tm* out) {
    if(out == nullptr) {
        return false;
    }
    ensureDockClockTimezone();
    const time_t now = time(nullptr);
    if(now < static_cast<time_t>(kValidUnixTime)) {
        return false;
    }
    localtime_r(&now, out);
    return true;
}

const char* weatherCodeLabel(uint16_t code) {
    if(code == 0) return "Clear";
    if(code <= 3) return "Clouds";
    if(code == 45 || code == 48) return "Fog";
    if((code >= 51 && code <= 57) || (code >= 61 && code <= 67)) return "Rain";
    if((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snow";
    if(code >= 80 && code <= 82) return "Showers";
    if(code >= 95) return "Storm";
    return "Weather";
}

bool jsonNumberBetween(const String& json, const char* key, int start, int end, float* out) {
    if(key == nullptr || out == nullptr || start < 0) {
        return false;
    }
    const int boundedEnd = end < 0 ? static_cast<int>(json.length()) : end;
    int pos = json.indexOf(key, static_cast<unsigned int>(start));
    if(pos < 0 || pos >= boundedEnd) {
        return false;
    }
    pos += static_cast<int>(strlen(key));
    while(pos < boundedEnd && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '[')) {
        ++pos;
    }

    char buffer[20] = {};
    uint8_t len = 0;
    while(pos < boundedEnd && len + 1 < sizeof(buffer)) {
        const char c = json[pos];
        if((c >= '0' && c <= '9') || c == '-' || c == '+') {
            buffer[len++] = c;
        } else if(c == '.') {
            buffer[len++] = c;
        } else {
            break;
        }
        ++pos;
    }
    if(len == 0) {
        return false;
    }
    *out = static_cast<float>(atof(buffer));
    return true;
}

bool updateDockWeatherFromJson(const String& payload) {
    const int currentStart = payload.indexOf("\"current\":");
    const int hourlyStart = payload.indexOf("\"hourly\":");
    if(currentStart < 0) {
        return false;
    }

    float temperature = 0.0f;
    float weatherCode = 0.0f;
    if(!jsonNumberBetween(payload, "\"temperature_2m\":", currentStart, hourlyStart, &temperature) ||
       !jsonNumberBetween(payload, "\"weather_code\":", currentStart, hourlyStart, &weatherCode)) {
        return false;
    }

    float precip = -1.0f;
    if(hourlyStart >= 0) {
        (void)jsonNumberBetween(payload, "\"precipitation_probability\":[", hourlyStart, -1, &precip);
    }

    app.dockWeather.valid = true;
    app.dockWeather.temperatureF = static_cast<int16_t>(lroundf(temperature));
    app.dockWeather.weatherCode = static_cast<uint16_t>(max<int>(0, static_cast<int>(lroundf(weatherCode))));
    app.dockWeather.precipitationPercent =
        precip >= 0.0f ? static_cast<int8_t>(clampValue(static_cast<int32_t>(lroundf(precip)), 0, 100)) : -1;
    app.dockWeather.updatedAt = millis();
    app.dockWeather.location = remote_config::kDockClockLocationName;
    return true;
}

String dockWeatherUrl() {
    String url = "http://api.open-meteo.com/v1/forecast?latitude=";
    url += String(remote_config::kDockClockWeatherLatitude, 4);
    url += "&longitude=";
    url += String(remote_config::kDockClockWeatherLongitude, 4);
    url += "&current=temperature_2m,weather_code,is_day";
    url += "&hourly=temperature_2m,precipitation_probability,weather_code";
    url += "&temperature_unit=fahrenheit&forecast_hours=6&timezone=auto";
    return url;
}

void serviceDockClockTime(uint32_t now) {
    if(!remote_wifi_sta_connected()) {
        return;
    }
    ensureDockClockTimezone();
    if(app.dockClockTimeConfigured &&
       (now - app.dockClockLastNtpSyncAt) < remote_config::kDockClockNtpSyncMs) {
        return;
    }

    configTzTime(remote_config::kDockClockTimezone,
                 remote_config::kDockClockNtpServer1,
                 remote_config::kDockClockNtpServer2);
    app.dockClockTimeConfigured = true;
    app.dockClockLastNtpSyncAt = now;
}

bool remoteCanUseBackgroundNetwork() {
    return !systemOtaBusy() && !app.editing && !app.targetDirty && !isRunningState() && app.screen != Screen::Profiler &&
           !remote_pressure_ble_connected() &&
           static_cast<uint32_t>(millis() - app.lastControlActivityAt) >= kFirmwareCheckUserQuietMs;
}

void scheduleRemoteAutoOtaCheck(uint32_t now, uint32_t delayMs) {
    app.nextAutoOtaCheckAt = now + delayMs;
}

bool remoteAutoOtaDue(uint32_t now) {
    if(app.nextAutoOtaCheckAt == 0) {
        scheduleRemoteAutoOtaCheck(now, kAutoOtaFirstCheckDelayMs);
        return false;
    }
    return msUntilDeadline(now, app.nextAutoOtaCheckAt) == 0;
}

void systemOtaDownloadProgress(uint32_t written, uint32_t total, void*) {
    g_systemOta.downloadWritten = written;
    g_systemOta.downloadTotal = total;
    remote_runtime_notify_input();
}

void backgroundNetworkWorker(void*) {
    for(;;) {
        BackgroundNetworkRequest* request = nullptr;
        if(xQueueReceive(g_networkRequests, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if(request->kind == BackgroundNetworkKind::Weather) {
            WiFiClient client;
            HTTPClient http;
            http.setConnectTimeout(kDockWeatherHttpTimeoutMs);
            http.setTimeout(kDockWeatherHttpTimeoutMs);
            if(http.begin(client, request->url)) {
                request->httpCode = http.GET();
                if(request->httpCode == HTTP_CODE_OK && http.getSize() <= 16384) {
                    request->payload = http.getString();
                    request->ok = request->payload.length() <= 16384;
                }
                http.end();
            }
        } else if(request->kind == BackgroundNetworkKind::SystemOtaStage) {
            request->ok = sg98_ota_download::fetchManifestSpecForTarget(
                sg98_ota_source::latestManifestUrl(), "remote", sg98_version::buildEnv(),
                &request->manifest, &request->result);
            if(request->ok) {
                request->ok = sg98_ota_download::fetchManifestSpecForTarget(
                    sg98_ota_source::controllerManifestUrl(), "controller", "controller-universal-s3",
                    &request->controllerManifest, &request->controllerResult);
            }
            if(request->ok &&
               (request->manifest.version != request->controllerManifest.version ||
                !request->manifest.gitSha.equalsIgnoreCase(request->controllerManifest.gitSha))) {
                request->ok = false;
                request->result.message = "controller and remote releases differ";
            }
            const esp_partition_t* partition = request->ok
                                                   ? esp_ota_get_next_update_partition(nullptr)
                                                   : nullptr;
            request->controllerImageOffset = request->ok
                                                 ? (request->manifest.size + 4095U) & ~4095U
                                                 : 0;
            if(request->ok &&
               (partition == nullptr || request->controllerImageOffset > partition->size ||
                request->controllerManifest.size > partition->size - request->controllerImageOffset)) {
                request->ok = false;
                request->result.message = "inactive slot cannot stage both images";
            }
            if(request->ok) {
                sg98_ota_download::DownloadOptions options;
                options.expectedSize = request->manifest.size;
                options.expectedSha256 = request->manifest.sha256;
                options.version = request->manifest.version;
                options.progress = systemOtaDownloadProgress;
                g_systemOta.downloadPart = 1;
                g_systemOta.downloadWritten = 0;
                g_systemOta.downloadTotal = request->manifest.size;
                request->ok = sg98_ota_download::downloadHttpsToPartition(
                    request->manifest.imageUrl, partition, 0, options, &request->result);
            }
            if(request->ok) {
                sg98_ota_download::DownloadOptions options;
                options.expectedSize = request->controllerManifest.size;
                options.expectedSha256 = request->controllerManifest.sha256;
                options.version = request->controllerManifest.version;
                options.progress = systemOtaDownloadProgress;
                g_systemOta.downloadPart = 2;
                g_systemOta.downloadWritten = 0;
                g_systemOta.downloadTotal = request->controllerManifest.size;
                request->ok = sg98_ota_download::downloadHttpsToPartition(
                    request->controllerManifest.imageUrl, partition,
                    request->controllerImageOffset, options, &request->controllerResult);
                if(!request->ok) request->result.message = request->controllerResult.message;
            }
        } else {
            request->ok = sg98_ota_download::fetchManifestSpec(request->url,
                                                              &request->manifest,
                                                              &request->result);
        }
        // Queue ownership keeps Strings and UI state confined to their owning task.
        xQueueSend(g_networkResults, &request, portMAX_DELAY);
        remote_runtime_notify_input();
    }
}

bool queueBackgroundNetwork(BackgroundNetworkKind kind, const String& url) {
    if(g_networkPending) {
        return false;
    }
    if(g_networkRequests == nullptr) {
        g_networkRequests = xQueueCreate(1, sizeof(BackgroundNetworkRequest*));
        g_networkResults = xQueueCreate(1, sizeof(BackgroundNetworkRequest*));
        if(g_networkRequests == nullptr || g_networkResults == nullptr ||
           xTaskCreate(backgroundNetworkWorker, "remote-net", 12288, nullptr, 1, nullptr) != pdPASS) {
            if(g_networkRequests != nullptr) vQueueDelete(g_networkRequests);
            if(g_networkResults != nullptr) vQueueDelete(g_networkResults);
            g_networkRequests = nullptr;
            g_networkResults = nullptr;
            return false;
        }
    }
    auto* request = new(std::nothrow) BackgroundNetworkRequest;
    if(request == nullptr) {
        return false;
    }
    request->kind = kind;
    request->url = url;
    if(xQueueSend(g_networkRequests, &request, 0) != pdTRUE) {
        delete request;
        return false;
    }
    g_networkPending = true;
    return true;
}

void finishAutoOtaWifi(bool ok) {
    if(g_autoOtaWifiStartedAt == 0) {
        return;
    }
    if(g_autoOtaWifiGeneration == g_transportGeneration) {
        restoreOtaWifi(g_autoOtaWifiGuard);
    }
    g_autoOtaWifiStartedAt = 0;
    scheduleRemoteAutoOtaCheck(millis(), ok ? sg98_ota_update::kOnlineCheckIntervalMs
                                           : kAutoOtaRetryAfterFailureMs);
}

void serviceBackgroundNetworkResults() {
    BackgroundNetworkRequest* request = nullptr;
    if(g_networkResults == nullptr || xQueueReceive(g_networkResults, &request, 0) != pdTRUE) {
        return;
    }
    g_networkPending = false;
    if(request->kind == BackgroundNetworkKind::Weather) {
        app.dockClockLastWeatherHttpCode = static_cast<int16_t>(request->httpCode);
        if(request->ok && updateDockWeatherFromJson(request->payload)) {
            app.dockClockLastWeatherOkAt = millis();
            app.redraw = true;
        } else if(request->httpCode == HTTP_CODE_OK) {
            app.dockClockLastWeatherHttpCode = -2;
        }
    } else {
        app.otaUpdate.lastAttemptOk = request->ok;
        app.otaUpdate.message = request->result.message;
        if(request->ok) {
            const auto& spec = request->manifest;
            app.otaUpdate.checked = true;
            app.otaUpdate.lastSuccessAt = millis();
            app.otaUpdate.latestVersion = spec.version;
            app.otaUpdate.latestGitSha = spec.gitSha;
            app.otaUpdate.latestGitDirty = spec.gitDirty;
            app.otaUpdate.latestGitDirtyPresent = spec.gitDirtyPresent;
            app.otaUpdate.latestGeneratedAtUtc = spec.generatedAtUtc;
            app.otaUpdate.latestImageSha256 = spec.sha256;
            app.otaUpdate.latestImageSize = spec.size;
            app.otaUpdate.updateAvailable = sg98_ota_update::manifestLooksNewer(spec);
            app.otaUpdate.message = app.otaUpdate.updateAvailable ? "update available" : "current";
        }
        if(request->kind == BackgroundNetworkKind::SystemOtaStage) {
            if(g_systemOta.flow.stage == sg98_system_ota::Stage::DownloadFiles) {
                if(request->ok) {
                    g_systemOta.remoteManifest = request->manifest;
                    g_systemOta.controllerManifest = request->controllerManifest;
                    g_systemOta.controllerImageOffset = request->controllerImageOffset;
                    g_systemOta.expectedVersion = request->controllerManifest.version;
                    g_systemOta.expectedSha = request->controllerManifest.gitSha;
                    g_systemOta.detail = "Files verified; reconnecting controller";
                    g_systemOta.flow.receive(sg98_system_ota::Event::FilesReady, millis());
                    switchTransportNow(LinkTransport::EspNow);
                    app.transportOverrideActive = true;
                } else {
                    finishSystemOta(false, String("Download: ") + request->result.message);
                }
            }
        } else {
            finishAutoOtaWifi(request->ok);
        }
        if(app.screen == Screen::About) app.redraw = true;
    }
    delete request;
    if(g_systemOta.restoreTransportPending) {
        g_systemOta.restoreTransportPending = false;
        if(g_systemOta.previousTransport != LinkTransport::None) {
            switchTransportNow(g_systemOta.previousTransport);
            app.transportOverrideActive = g_systemOta.previousOverride;
        }
    }
}

void serviceFirmwareUpdateCheck(uint32_t now) {
    if(g_autoOtaWifiStartedAt != 0) {
        if(g_autoOtaWifiGeneration != g_transportGeneration) {
            g_autoOtaWifiStartedAt = 0;
        } else if(!remote_wifi_sta_connected()) {
            if(static_cast<uint32_t>(now - g_autoOtaWifiStartedAt) >= 30000UL) {
                app.otaUpdate.message = "wifi connect timeout";
                finishAutoOtaWifi(false);
            }
            return;
        }
    }

    if(g_networkPending || !remoteCanUseBackgroundNetwork()) {
        return;
    }
    if(ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        return;
    }

    if(!remote_wifi_sta_connected()) {
        if(!remoteAutoOtaDue(now)) {
            return;
        }
        if(isEspNowLinkOnline()) {
            scheduleRemoteAutoOtaCheck(now, kAutoOtaRetryAfterFailureMs);
            return;
        }
        if(!sg98_ota_update::shouldCheck(app.otaUpdate, now)) {
            scheduleRemoteAutoOtaCheck(now, sg98_ota_update::kOnlineCheckIntervalMs);
            return;
        }
        if(!remote_wifi_has_saved_network()) {
            scheduleRemoteAutoOtaCheck(now, kAutoOtaRetryAfterNoSavedWifiMs);
            return;
        }

        g_autoOtaWifiGuard.previousTransport = app.currentTransport;
        g_autoOtaWifiGuard.switchedTransport = app.currentTransport != LinkTransport::WebSocket;
        switchTransportNow(LinkTransport::WebSocket);
        g_autoOtaWifiGeneration = g_transportGeneration;
        g_autoOtaWifiStartedAt = millis();
        return;
    } else if(g_autoOtaWifiStartedAt == 0) {
        struct tm timeInfo = {};
        const bool internetSynced = localClockTime(&timeInfo) || app.dockWeather.valid;
        if(!internetSynced) {
            return;
        }
    }

    if(!sg98_ota_update::shouldCheck(app.otaUpdate, now)) {
        finishAutoOtaWifi(true);
        return;
    }

    if(queueBackgroundNetwork(BackgroundNetworkKind::OtaManifest, sg98_ota_source::latestManifestUrl())) {
        app.otaUpdate.lastAttemptAt = now;
    }
}

void serviceDockClockWeather(uint32_t now, bool quickRetry = false) {
    if(g_networkPending || !remote_wifi_sta_connected()) {
        return;
    }
    if(ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        return;
    }
    const uint32_t retryMs = app.dockWeather.valid
                                 ? remote_config::kDockClockWeatherFetchMs
                                 : (quickRetry ? kDockWeatherQuickRetryMs : kDockWeatherRetryMs);
    if(app.dockClockLastWeatherFetchAt != 0 &&
       (now - app.dockClockLastWeatherFetchAt) < retryMs) {
        return;
    }
    if(queueBackgroundNetwork(BackgroundNetworkKind::Weather, dockWeatherUrl())) {
        app.dockClockLastWeatherFetchAt = now;
        app.dockClockLastWeatherHttpCode = 0;
    }
}

void serviceBackgroundDockClockWeather(uint32_t now) {
    if(app.dockClockActive || !remoteCanUseBackgroundNetwork()) {
        return;
    }
    serviceDockClockWeather(now, true);
}

bool buildLocalEnvironmentFrame(sg98_protocol::EnvironmentFrame* out) {
    if(out == nullptr) {
        return false;
    }

    *out = {};
    const time_t unixNow = time(nullptr);
    if(unixNow >= static_cast<time_t>(kValidUnixTime)) {
        out->timeValid = true;
        out->unixTime = static_cast<uint32_t>(unixNow);
    }

    if(app.dockWeather.valid) {
        out->weatherValid = true;
        out->temperatureF = app.dockWeather.temperatureF;
        out->weatherCode = app.dockWeather.weatherCode;
        out->precipitationPercent = app.dockWeather.precipitationPercent;
        out->location = app.dockWeather.location;
    }

    return out->timeValid || out->weatherValid;
}

void shareLocalEnvironmentWithController(uint32_t now) {
    if(!isLinkOnline() || !app.hasControllerStatus || !app.controllerStatus.envSyncFieldsPresent) {
        return;
    }
    if(app.lastEnvironmentShareAt != 0 && (now - app.lastEnvironmentShareAt) < kEnvironmentShareRetryMs) {
        return;
    }

    sg98_protocol::EnvironmentFrame frame;
    if(!buildLocalEnvironmentFrame(&frame)) {
        return;
    }

    const bool controllerNeedsTime = frame.timeValid && !app.controllerStatus.envTimeValid;
    const bool controllerNeedsWeather = frame.weatherValid && !app.controllerStatus.envWeatherValid;
    if(!controllerNeedsTime && !controllerNeedsWeather) {
        return;
    }

    if(sendRaw(sg98_protocol::environment(frame))) {
        app.lastEnvironmentShareAt = now;
    }
}

void serviceControllerEnvironmentSync(uint32_t now) {
    if(systemOtaBusy()) return;
    if(!isLinkOnline()) {
        return;
    }

    shareLocalEnvironmentWithController(now);

    struct tm timeInfo = {};
    const bool needsTime = !localClockTime(&timeInfo);
    const bool needsWeather = !app.dockWeather.valid ||
                              (app.dockWeather.updatedAt != 0 &&
                               (now - app.dockWeather.updatedAt) >= remote_config::kDockClockWeatherFetchMs);
    if(!needsTime && !needsWeather) {
        return;
    }

    const uint32_t interval = needsTime || !app.dockWeather.valid
                                  ? kControllerEnvironmentRetryMs
                                  : remote_config::kDockClockWeatherFetchMs;
    if(app.lastControllerEnvironmentRequestAt != 0 &&
       (now - app.lastControllerEnvironmentRequestAt) < interval) {
        return;
    }
    requestControllerEnvironment(true);
}

bool dockClockAllowed() {
    return app.screen != Screen::Profiler && !app.editing && !app.targetDirty &&
           !isRunningState() && !remote_pressure_ble_connected();
}

void exitDockClock() {
    if(!app.dockClockActive) {
        return;
    }
    app.dockClockActive = false;
    app.dockClockLastRedrawAt = 0;
    applyCurrentBacklightLevel(true);
    app.redraw = true;
}

void serviceDockClock(uint32_t now) {
    if(!dockClockAllowed()) {
        exitDockClock();
        return;
    }

    serviceDockClockTime(now);
    if(!app.dockClockActive) {
        if((now - app.lastUserActivityAt) < remote_config::kDockClockAfterMs) {
            return;
        }
        app.dockClockActive = true;
        app.dockClockLastRedrawAt = 0;
        applyBacklightLevel(dockClockBacklightLevel(), true);
        app.redraw = true;
    }

    serviceDockClockWeather(now);
    if((now - app.dockClockLastRedrawAt) >= remote_config::kDockClockRedrawMs) {
        app.dockClockLastRedrawAt = now;
        app.redraw = true;
    }
}

void shortenWaitForDockClock(uint32_t now, uint32_t* waitMs) {
    if(waitMs == nullptr || !dockClockAllowed()) {
        return;
    }
    if(app.dockClockActive) {
        shortenWaitForDeadline(now, app.dockClockLastRedrawAt + remote_config::kDockClockRedrawMs, waitMs);
        return;
    }
    shortenWaitForDeadline(now, app.lastUserActivityAt + remote_config::kDockClockAfterMs, waitMs);
}

bool statusMotorUnavailable(const sg98_protocol::StatusFrame& status) {
    return status.comm == "DISCONNECTED" ||
           status.motor == "NOT_CONNECTED" ||
           status.motor == "FAULT";
}

bool isRunActionAvailable() {
    return !systemOtaBusy() && isLinkOnline();
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

int32_t rpmMagnitude(int32_t rpm) {
    return rpm < 0 ? -rpm : rpm;
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

bool reverseRpmDisplayActive() {
    return !isGbwState() && isRunningState() && homeWheelDisplayRpm() < 0;
}

uint16_t reverseRpmAccentColor() {
    return blendColor565(kColorDanger, kColorWarn, 0.28f);
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

    if(reverseRpmDisplayActive()) {
        const uint16_t reverseAccent = reverseRpmAccentColor();
        palette.background = blendColor565(kColorBg, reverseAccent, 0.18f);
        palette.panel = blendColor565(kColorPanel, reverseAccent, 0.32f);
        palette.panelHi = blendColor565(kColorPanelHi, reverseAccent, 0.30f);
        palette.accent = reverseAccent;
        palette.accent2 = blendColor565(kColorWarn, kColorDanger, 0.36f);
        palette.muted = blendColor565(kColorMuted, reverseAccent, 0.22f);
        palette.modeFill = blendColor565(kColorDanger, kColorWarn, 0.34f);
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

bool batteryUsbPowerHold(const RemoteBatteryStatus& battery) {
    return battery.usbPresent && (battery.standby || batteryTopOffUsbHold(battery));
}

uint16_t batteryStateAccent(const RemoteBatteryStatus& battery, uint16_t fallbackFill = kColorMuted) {
    if(!battery.supported) {
        return fallbackFill;
    }
    if(batteryUsbPowerHold(battery)) {
        return kColorPowerHold;
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
    return batteryStateAccent(battery, fallbackFill);
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

uint8_t dockClockBacklightLevel() {
    const uint8_t normalLevel = currentBacklightLevel();
    const uint8_t dockLevel = static_cast<uint8_t>(
        clampValue(static_cast<int32_t>(remote_config::kDockClockBacklightLevel), 1, 255));
    return dockLevel < normalLevel ? dockLevel : normalLevel;
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
        case LOCAL_DRIVE_KRS: return LOCAL_DRIVE_KRS;
        case LOCAL_DRIVE_XB130: return LOCAL_DRIVE_XB130;
        case LOCAL_DRIVE_SEW: return LOCAL_DRIVE_SEW;
        case LOCAL_DRIVE_TSE: return LOCAL_DRIVE_TSE;
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
        case LOCAL_DRIVE_KRS: return "KRS130ST";
        case LOCAL_DRIVE_XB130: return "XB130";
        case LOCAL_DRIVE_SEW: return "SEW";
        case LOCAL_DRIVE_TSE: return "TSE";
        case LOCAL_DRIVE_AUTO:
        default:
            return "AUTO";
    }
}

bool forcedDriveForArg(const char* arg,
                       LocalDriveSelection* selection,
                       PendingActionKind* pendingKind,
                       const char** expectedLabel) {
    if(arg == nullptr) {
        return false;
    }
    if(strcmp(arg, "tse") == 0) {
        if(selection != nullptr) *selection = LOCAL_DRIVE_TSE;
        if(pendingKind != nullptr) *pendingKind = PendingActionKind::DriveTse;
        if(expectedLabel != nullptr) *expectedLabel = "TSE";
        return true;
    }
    if(strcmp(arg, "jmc") == 0) {
        if(selection != nullptr) *selection = LOCAL_DRIVE_JMC;
        if(pendingKind != nullptr) *pendingKind = PendingActionKind::DriveJmc;
        if(expectedLabel != nullptr) *expectedLabel = "JMC";
        return true;
    }
    if(strcmp(arg, "rt") == 0) {
        if(selection != nullptr) *selection = LOCAL_DRIVE_RT;
        if(pendingKind != nullptr) *pendingKind = PendingActionKind::DriveRt;
        if(expectedLabel != nullptr) *expectedLabel = "RT";
        return true;
    }
    if(strcmp(arg, "krs") == 0 || strcmp(arg, "krs130st") == 0 || strcmp(arg, "krs130st-m07725") == 0) {
        if(selection != nullptr) *selection = LOCAL_DRIVE_KRS;
        if(pendingKind != nullptr) *pendingKind = PendingActionKind::DriveKrs;
        if(expectedLabel != nullptr) *expectedLabel = "KRS130ST";
        return true;
    }
    if(strcmp(arg, "xb130") == 0 || strcmp(arg, "krs-xb130aea20025-shl") == 0) {
        if(selection != nullptr) *selection = LOCAL_DRIVE_XB130;
        if(pendingKind != nullptr) *pendingKind = PendingActionKind::DriveXb130;
        if(expectedLabel != nullptr) *expectedLabel = "XB130";
        return true;
    }
    if(strcmp(arg, "sew") == 0 || strcmp(arg, "sew-movilink") == 0 || strcmp(arg, "movilink") == 0) {
        if(selection != nullptr) *selection = LOCAL_DRIVE_SEW;
        if(pendingKind != nullptr) *pendingKind = PendingActionKind::DriveSew;
        if(expectedLabel != nullptr) *expectedLabel = "SEW";
        return true;
    }
    return false;
}

bool isRadioModeItem(const SettingItem& item) {
    return item.menuNum == 1 && item.itemIdx == RADIOMODE;
}

bool isGaugeSetItem(const SettingItem& item) {
    return item.menuNum == 1 && item.itemIdx == GAUGESET;
}

bool pendingGaugeSetWriteValue(int32_t* valueOut = nullptr) {
    for(const PendingMenuWrite& pending : app.pendingMenuWrites) {
        if(pending.active && pending.menuNum == 1 && pending.itemIdx == GAUGESET) {
            if(valueOut != nullptr) {
                *valueOut = pending.value;
            }
            return true;
        }
    }
    return false;
}

bool gaugeSetMenuValueKnown() {
    return app.controllerMenuSynced[1] || pendingGaugeSetWriteValue();
}

int32_t gaugeSetMenuValue() {
    int32_t pendingValue = 0;
    if(pendingGaugeSetWriteValue(&pendingValue)) {
        return pendingValue;
    }
    if(app.controllerMenuSynced[1]) {
        return app.controllerMenuValues[1][GAUGESET];
    }
    return 0;
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

bool activeDriveIsJmc() {
    if(app.hasStatus && app.status.drive.length() > 0) {
        return app.status.drive == "JMC";
    }
    return currentLocalDriveSelection() == LOCAL_DRIVE_JMC;
}

bool settingItemVisible(const SettingItem& item) {
    if(item.menuNum == 1 && item.itemIdx == SETMOTORRAMP) {
        return activeDriveIsJmc();
    }
    if(item.action != nullptr && strcmp(item.action, "jmc_baud") == 0) {
        return activeDriveIsJmc();
    }
    return true;
}

const SettingItem* filteredSettingItems(const SettingItem* rawItems, size_t rawCount, size_t* count) {
    static SettingItem filtered[16];
    size_t visibleCount = 0;
    for(size_t idx = 0; idx < rawCount && visibleCount < arrayCount(filtered); ++idx) {
        if(settingItemVisible(rawItems[idx])) {
            filtered[visibleCount++] = rawItems[idx];
        }
    }
    if(count != nullptr) {
        *count = visibleCount;
    }
    return filtered;
}

bool macAddressEqual(const uint8_t* lhs, const uint8_t* rhs) {
    return lhs != nullptr && rhs != nullptr && memcmp(lhs, rhs, 6) == 0;
}

bool macAddressIsZero(const uint8_t* mac) {
    if(mac == nullptr) {
        return true;
    }
    for(uint8_t i = 0; i < 6; ++i) {
        if(mac[i] != 0) {
            return false;
        }
    }
    return true;
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
    app.lastLinkActivityAt = 0;
    app.lastAckAt = 0;
    app.lastStatusAt = 0;
    app.espNowPeerConfigured = false;
    memset(app.espNowPeerMac, 0, sizeof(app.espNowPeerMac));
    app.lastEspNowDiscoveryAt = 0;
    (void)setEspNowChannel(remote_config::kEspNowChannel);
}

void setLockedEspNowPeer(const uint8_t* mac) {
    if(mac == nullptr) {
        clearLockedEspNowPeer();
        return;
    }

    const bool changed = !app.espNowPeerConfigured ||
                         !macAddressEqual(app.espNowPeerMac, mac);
    if(changed) {
        app.lastLinkActivityAt = 0;
        app.lastAckAt = 0;
        app.lastStatusAt = 0;
        app.lastEspNowDiscoveryAt = 0;
    }
    memcpy(app.espNowPeerMac, mac, sizeof(app.espNowPeerMac));
    app.espNowPeerConfigured = true;
}

uint8_t normalizeEspNowChannel(int32_t channel) {
    if(channel < kEspNowMinChannel || channel > kEspNowMaxChannel) {
        return remote_config::kEspNowChannel;
    }
    return static_cast<uint8_t>(channel);
}

const DiscoveredEspNowPeer* discoveredPeerForMac(const uint8_t* mac) {
    if(mac == nullptr) {
        return nullptr;
    }

    for(uint8_t i = 0; i < kEspNowPeerSlots; ++i) {
        if(app.espNowPeers[i].valid && macAddressEqual(app.espNowPeers[i].mac, mac)) {
            return &app.espNowPeers[i];
        }
    }
    return nullptr;
}

uint8_t activeEspNowPeerChannel() {
    const DiscoveredEspNowPeer* peer = discoveredPeerForMac(activeEspNowPeerMac());
    return peer != nullptr ? normalizeEspNowChannel(peer->channel) : 0;
}

bool setEspNowChannel(uint8_t channel) {
    channel = normalizeEspNowChannel(channel);
    if(app.espNowChannel == channel) {
        return true;
    }

    if(app.espNowReady &&
       esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }

    app.espNowChannel = channel;
    return true;
}

uint8_t nextEspNowDiscoveryChannel() {
    uint8_t channel = app.espNowChannel;
    if(channel < kEspNowMinChannel || channel >= kEspNowMaxChannel) {
        return kEspNowMinChannel;
    }
    return static_cast<uint8_t>(channel + 1);
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
        return "ESP-NOW";
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
        return "WIFI";
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
    out->burrGapFieldsPresent = status.burrGapFieldsPresent;
    out->burrGapEnabled = status.burrGapEnabled;
    out->burrGapKnown = status.burrGapKnown;
    out->burrGapLive = status.burrGapLive;
    out->burrGapUm = status.burrGapUm;
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
    out->burrGapFieldsPresent = in.burrGapFieldsPresent;
    out->burrGapEnabled = in.burrGapEnabled;
    out->burrGapKnown = in.burrGapKnown;
    out->burrGapLive = in.burrGapLive;
    out->burrGapUm = in.burrGapUm;
}

void captureSnapshot(SleepSnapshot* snapshot, bool includeStatus) {
    if(snapshot == nullptr) {
        return;
    }

    snapshot->magic = kSleepSnapshotMagic;
    snapshot->version = kSleepSnapshotVersion;
    snapshot->screen = static_cast<uint8_t>(app.screen);
    memcpy(snapshot->selection, app.selection, sizeof(snapshot->selection));
    if(app.screen == Screen::Profiler) {
        snapshot->screen = static_cast<uint8_t>(Screen::Home);
    }
    snapshot->manualRpm = app.manualRpm;
    snapshot->weightMg = app.weightMg;
    snapshot->localInvertScroll = app.localInvertScroll;
    snapshot->localUiSounds = app.localUiSounds;
    snapshot->localDriveSelection = app.localDriveSelection;
    memcpy(snapshot->menuValues, app.menuValues, sizeof(snapshot->menuValues));
    snapshot->menuValues[1][GAUGESET] = 0;
    snapshot->menuSyncedMask = 0;
    for(uint8_t menu = 0; menu < kControllerMenuSlots; ++menu) {
        if(app.menuSynced[menu]) {
            snapshot->menuSyncedMask |= static_cast<uint8_t>(1u << menu);
        }
    }
    snapshot->transport = static_cast<uint8_t>(LinkTransport::EspNow);
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
    if(screenIndex <= static_cast<uint8_t>(Screen::About)) {
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
    app.menuValues[1][GAUGESET] = 0;
    app.controllerMenuValues[1][GAUGESET] = 0;
    app.controllerMenuSynced[1] = false;
    for(uint8_t menu = 0; menu < kControllerMenuSlots; ++menu) {
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
        *restoredTransport = LinkTransport::EspNow;
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

    const bool leavingProfiler = app.screen == Screen::Profiler;
    app.screen = Screen::Home;
    app.menuRollOffset = 0.0f;
    app.editing = false;
    app.redraw = true;
    if(leavingProfiler) {
        leavePressureProfiler();
    }
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
    const uint32_t now = millis();
    if(static_cast<int32_t>(now - g_persistentStateDueAt) < 0) {
        return;
    }
    if(roundUiRecentActivity(kPersistentStateUiQuietMs)) {
        g_persistentStateDueAt = now + kPersistentStateDebounceMs;
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
    addDeepSleepWakePin(&mask, pins.secondaryButton, RemoteWakeSource::SecondaryButton);
    addDeepSleepWakePin(&mask, pins.encoderSwitch, RemoteWakeSource::EncoderSwitch);
    addDeepSleepWakePin(&mask, pins.imuInt, RemoteWakeSource::Motion);
    return mask;
}

bool idleDeepSleepSupported() {
    return remote_board_support_present(remote_board_feature_map().power.deepSleep) &&
           deepSleepWakeMask() != 0;
}

bool touchWakeNeedsLightSleep() {
    if(!remote_board_wake_supported(RemoteWakeSource::Touch)) {
        return false;
    }

    const int8_t touchInt = remote_board_pins().touchInt;
    return touchInt >= 0 && !esp_sleep_is_valid_wakeup_gpio(static_cast<gpio_num_t>(touchInt));
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

bool roundUiRecentActivity(uint32_t quietMs) {
    if(!useRoundNavigation()) {
        return false;
    }

    const uint32_t now = millis();
    return touchTracker.active || app.targetDirty || app.editing ||
           fabsf(app.menuRollOffset) >= 0.5f ||
           (now - app.lastUserActivityAt) < quietMs;
}

bool shouldYieldTransportToUi() {
    if(kDisableTransportPerfTest || !useRoundNavigation()) {
        return false;
    }

    if(touchTracker.active || app.targetDirty || app.editing ||
       fabsf(app.menuRollOffset) >= 0.5f) {
        return true;
    }

    if(pendingControllerSyncActive()) {
        return false;
    }

    if((millis() - app.lastUserActivityAt) < kTransportUiYieldMs) {
        return true;
    }

    if(app.currentTransport == LinkTransport::WebSocket ||
       app.pendingTransport == LinkTransport::WebSocket ||
       remote_wifi_is_active()) {
        return false;
    }

    if(app.screen != Screen::Home || app.editing) {
        return false;
    }

    if(homeGaugeFastPollActive()) {
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
        if(homeGaugeFastPollActive()) {
            return kHomeGaugeStatusPollMs;
        }

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

void markUserActivity(uint32_t now) {
    app.lastUserActivityAt = now;
}

bool handleMotionHomeGesture(uint32_t now) {
    if(!kMotionHomeEnabled) {
        app.motionHomeCandidateAt = 0;
        return false;
    }

    if(app.screen == Screen::Profiler) {
        app.motionHomeCandidateAt = 0;
        return false;
    }

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

bool controllerOtaReady() {
    return app.hasStatus &&
           app.status.controllerOtaReadyFieldsPresent &&
           app.status.controllerOtaReady;
}

bool controllerOtaActionBusy() {
    return app.controllerOtaActionBusy && millis() < app.controllerOtaActionUntil;
}

bool controllerOtaDownloadArmed() {
    return app.controllerOtaDownloadArmed && millis() < app.controllerOtaDownloadArmUntil;
}

void clearControllerOtaBusy() {
    app.controllerOtaActionBusy = false;
    app.controllerOtaActionStartedAt = 0;
    app.controllerOtaActionUntil = 0;
    app.lastControllerOtaProgressAt = 0;
    app.controllerOtaProgressLabel = "";
    if(app.flashTitle.startsWith("CONTROLLER OTA")) {
        app.flashUntil = 0;
        app.flashTitle = "";
        app.flashSubtitle = "";
    }
    app.redraw = true;
}

void clearControllerOtaDownloadArm() {
    app.controllerOtaDownloadArmed = false;
    app.controllerOtaDownloadArmUntil = 0;
    app.redraw = true;
}

void armControllerOtaDownload() {
    app.controllerOtaDownloadArmed = true;
    app.controllerOtaDownloadArmUntil = millis() + kControllerOtaDownloadConfirmMs;
    app.redraw = true;
}

bool isControllerOtaActionItem(const SettingItem& item) {
    return item.action != nullptr && strcmp(item.action, "controller_ota") == 0;
}

String controllerOtaActionValue() {
    if(controllerOtaReady()) {
        return "READY";
    }
    if(controllerOtaActionBusy()) {
        return "BUSY";
    }
    if(controllerOtaDownloadArmed()) {
        return "DOWNLOAD";
    }
    return "CHECK";
}

int applyDialDirection(int delta) {
    if(delta == 0) {
        return 0;
    }
    return -delta;
}

int conditionHardwareDialForMenu(int delta) {
    if(delta == 0) {
        return 0;
    }

    if(!remote_board_info().hasEncoder || app.screen == Screen::Home || app.editing) {
        app.menuEncoderRemainder = 0;
        return delta;
    }

    if((app.menuEncoderRemainder > 0 && delta < 0) ||
       (app.menuEncoderRemainder < 0 && delta > 0)) {
        app.menuEncoderRemainder = 0;
    }

    app.menuEncoderRemainder =
        static_cast<int16_t>(app.menuEncoderRemainder + clampValue(delta, -kMenuEncoderStepDivisor, kMenuEncoderStepDivisor));
    const int steps = app.menuEncoderRemainder / kMenuEncoderStepDivisor;
    if(steps != 0) {
        app.menuEncoderRemainder =
            static_cast<int16_t>(app.menuEncoderRemainder - (steps * kMenuEncoderStepDivisor));
    }
    return steps;
}

bool isVisibleControllerMenuScreen(Screen screen, uint8_t menuNum) {
    switch(menuNum) {
        case 1: return screen == Screen::Main || screen == Screen::Gbw || screen == Screen::Network ||
                       screen == Screen::AudioDiag;
        case 2: return screen == Screen::Purge;
        case 3: return screen == Screen::Gbw;
        case 5: return screen == Screen::Network;
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
           lhs.preferredJmcBaud == rhs.preferredJmcBaud &&
           lhs.envSyncFieldsPresent == rhs.envSyncFieldsPresent &&
           lhs.envTimeValid == rhs.envTimeValid &&
           lhs.envWeatherValid == rhs.envWeatherValid &&
           lhs.burrGapFieldsPresent == rhs.burrGapFieldsPresent &&
           lhs.burrGapEnabled == rhs.burrGapEnabled &&
           lhs.burrGapKnown == rhs.burrGapKnown &&
           lhs.burrGapLive == rhs.burrGapLive &&
           lhs.burrGapUm == rhs.burrGapUm &&
           lhs.controllerOtaReadyFieldsPresent == rhs.controllerOtaReadyFieldsPresent &&
           lhs.controllerOtaReady == rhs.controllerOtaReady;
}

bool menuFrameMatchesCache(const sg98_protocol::MenuFrame& frame) {
    if(frame.menuNum == 0 || frame.menuNum > kControllerMenuMax || !app.menuSynced[frame.menuNum]) {
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
        return status.state != previous.state || status.error != previous.error ||
               status.burrGapKnown != previous.burrGapKnown ||
               status.burrGapLive != previous.burrGapLive ||
               status.burrGapUm != previous.burrGapUm;
    }

    if(status.state == "GRINDING" || status.state == "GRINDING_GBW" || status.state == "PURGING") {
        return status.state != previous.state || status.error != previous.error ||
               status.currentRpm != previous.currentRpm ||
               status.currentWeightMg != previous.currentWeightMg ||
               status.setRpm != previous.setRpm ||
               status.setWeightMg != previous.setWeightMg ||
               status.burrGapKnown != previous.burrGapKnown ||
               status.burrGapLive != previous.burrGapLive ||
               status.burrGapUm != previous.burrGapUm ||
               status.motor != previous.motor || status.drive != previous.drive;
    }

    return status.state != previous.state || status.error != previous.error ||
           status.motor != previous.motor || status.drive != previous.drive ||
           status.comm != previous.comm ||
           status.burrGapFieldsPresent != previous.burrGapFieldsPresent ||
           status.burrGapEnabled != previous.burrGapEnabled ||
           status.burrGapKnown != previous.burrGapKnown ||
           status.burrGapLive != previous.burrGapLive ||
           status.burrGapUm != previous.burrGapUm ||
           status.setRpm != previous.setRpm ||
           status.setWeightMg != previous.setWeightMg;
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

void showFlashScreen(const String& title, const String& subtitle, uint32_t durationMs, uint16_t accent) {
    app.flashTitle = title;
    app.flashSubtitle = subtitle;
    app.flashAccent = accent;
    app.flashUntil = millis() + durationMs;
    app.redraw = true;
}

String shortElapsedLabel(uint32_t elapsedMs) {
    const uint32_t elapsedSeconds = elapsedMs / 1000UL;
    String out = String(elapsedSeconds / 60UL);
    out += ':';
    const uint32_t seconds = elapsedSeconds % 60UL;
    if(seconds < 10UL) {
        out += '0';
    }
    out += String(seconds);
    return out;
}

String controllerOtaProgressSubtitle(uint32_t now) {
    String label = app.controllerOtaProgressLabel.length() > 0
                       ? app.controllerOtaProgressLabel
                       : app.flashSubtitle;
    if(label.length() == 0) {
        label = "Waiting for controller";
    }
    label += " ";
    label += shortElapsedLabel(app.controllerOtaActionStartedAt == 0
                                   ? 0
                                   : now - app.controllerOtaActionStartedAt);
    return label;
}

void updateControllerOtaProgressLabel(const String& label, uint32_t now = millis()) {
    app.controllerOtaProgressLabel = label;
    if(app.controllerOtaActionBusy) {
        app.flashSubtitle = controllerOtaProgressSubtitle(now);
        app.lastControllerOtaProgressAt = now;
        app.redraw = true;
    }
}

void serviceControllerOtaUi(uint32_t now) {
    if(app.controllerOtaActionBusy && msUntilDeadline(now, app.controllerOtaActionUntil) == 0) {
        clearControllerOtaBusy();
        clearControllerOtaDownloadArm();
        setToast("Controller OTA timeout", 1800);
    } else if(app.controllerOtaActionBusy &&
              (app.lastControllerOtaProgressAt == 0 ||
               now - app.lastControllerOtaProgressAt >= kControllerOtaProgressRedrawMs)) {
        app.flashSubtitle = controllerOtaProgressSubtitle(now);
        app.lastControllerOtaProgressAt = now;
        app.redraw = true;
    }
    if(app.controllerOtaDownloadArmed && msUntilDeadline(now, app.controllerOtaDownloadArmUntil) == 0) {
        clearControllerOtaDownloadArm();
        setToast("OTA download canceled", 1200);
    }
}

void serviceFlashScreen(uint32_t now) {
    if(app.flashUntil == 0 || now < app.flashUntil) {
        return;
    }
    app.flashUntil = 0;
    app.flashTitle = "";
    app.flashSubtitle = "";
    app.redraw = true;
}

void refreshBatteryStatus(bool force) {
    if(kDisableTransportPerfTest) {
        return;
    }

    const uint32_t now = millis();
    if(app.lastBatteryPollAt != 0 &&
       (now - app.lastBatteryPollAt) < remote_config::kBatteryStatusPollMs) {
        return;
    }
    if(!force && touchTracker.active) {
        return;
    }
    app.lastBatteryPollAt = now;
    const RemoteBatteryStatus latest = remote_board_battery_status();
    if(!batteryStatusEqual(latest, app.battery)) {
        const bool visualChanged = !batteryStatusVisualEqual(latest, app.battery);
        app.battery = latest;
        if(visualChanged) {
            app.redraw = true;
        }
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
            return filteredSettingItems(kGbwItems, arrayCount(kGbwItems), count);
        case Screen::Network:
            if(count != nullptr) *count = arrayCount(kNetworkItems);
            return kNetworkItems;
        case Screen::AudioDiag:
            if(count != nullptr) *count = arrayCount(kAudioItems);
            return kAudioItems;
        case Screen::Drive:
            return filteredSettingItems(kDriveItems, arrayCount(kDriveItems), count);
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
        case Screen::Profiler:
        case Screen::About:
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
    app.selection[static_cast<uint8_t>(Screen::Profiler)] = 0;
    app.selection[static_cast<uint8_t>(Screen::About)] = 0;
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
    if(isGaugeSetItem(item)) {
        return gaugeSetMenuValue();
    }
    if(item.menuNum == 0) {
        return 0;
    }
    return app.menuValues[item.menuNum][item.itemIdx];
}

bool itemUsesRpmDisplay(const SettingItem& item) {
    return item.unit != nullptr && strcmp(item.unit, "rpm") == 0;
}

int32_t displayValueForItem(const SettingItem& item, int32_t value) {
    return itemUsesRpmDisplay(item) ? rpmMagnitude(value) : value;
}

String gaugeGapLabelFromMenuValue(int32_t value) {
    return String(static_cast<long>(value) * 10L);
}

String formatValueLabel(const SettingItem& item, int32_t value) {
    if(isRadioModeItem(item)) {
        return radioModeValueLabel(value);
    }
    if(isGaugeSetItem(item)) {
        return gaugeGapLabelFromMenuValue(value);
    }
    if(usesChoiceEditor(item)) {
        return value != 0 ? toggleOnLabel(item) : toggleOffLabel(item);
    }

    if(item.itemIdx == GBW_SLOW_MG) {
        return weightLabel(value);
    }

    String out = String(displayValueForItem(item, value));
    if(item.unit != nullptr && item.unit[0] != '\0') {
        out += ' ';
        out += item.unit;
    }
    return out;
}

String formatSettingValue(const SettingItem& item) {
    if(item.kind == ItemKind::Action || item.kind == ItemKind::Danger) {
        if(item.action != nullptr && strcmp(item.action, "about") == 0) {
            return sg98_version::shortVersion();
        }
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
        if(isControllerOtaActionItem(item)) {
            return controllerOtaActionValue();
        }
        if(item.action != nullptr && strcmp(item.action, "screen") == 0) {
            if(item.actionArg != nullptr && strcmp(item.actionArg, "about") == 0) {
                return controllerOtaReady() ? "OTA READY" : sg98_version::shortVersion();
            }
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
                LocalDriveSelection selection = LOCAL_DRIVE_AUTO;
                if(forcedDriveForArg(item.actionArg, &selection, nullptr, nullptr) &&
                   currentLocalDriveSelection() == selection) {
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

    if(isGaugeSetItem(item) && !gaugeSetMenuValueKnown()) {
        return "--";
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

void serviceDeferredHomeStatusRedraw() {
    if(!app.deferredHomeStatusRedraw) {
        return;
    }

    if(app.screen != Screen::Home) {
        app.deferredHomeStatusRedraw = false;
        return;
    }

    if(roundUiInteractionActive()) {
        return;
    }

    app.deferredHomeStatusRedraw = false;
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

NetworkBadgeMode currentNetworkBadgeMode() {
    if(remote_wifi_is_active()) {
        if(remote_wifi_sta_connected()) {
            return NetworkBadgeMode::WifiLan;
        }
        if(remote_wifi_ap_running()) {
            return NetworkBadgeMode::WifiAp;
        }
        return NetworkBadgeMode::WifiJoin;
    }
    if(app.currentTransport == LinkTransport::EspNow) {
        return NetworkBadgeMode::EspNow;
    }
    return NetworkBadgeMode::Off;
}

String networkBadgeLabel(NetworkBadgeMode mode) {
    switch(mode) {
        case NetworkBadgeMode::WifiLan:
            return isLinkOnline() ? "WIFI LINK" : "WIFI NO CTRL";
        case NetworkBadgeMode::WifiAp:
            return remote_wifi_ap_station_count() > 0
                       ? String("WIFI AP ") + String(static_cast<unsigned>(remote_wifi_ap_station_count()))
                       : String("WIFI AP");
        case NetworkBadgeMode::WifiJoin:
            return "WIFI JOIN";
        case NetworkBadgeMode::EspNow:
            return isLinkOnline() ? "ESP-NOW LINK" : "ESP-NOW SEARCH";
        case NetworkBadgeMode::Off:
        default:
            return "NET OFF";
    }
}

uint16_t networkBadgeFill(NetworkBadgeMode mode) {
    switch(mode) {
        case NetworkBadgeMode::WifiLan:
            return isLinkOnline() ? kColorGood : kColorWarn;
        case NetworkBadgeMode::WifiAp:
            return kColorWarn;
        case NetworkBadgeMode::WifiJoin:
            return kColorAccent2;
        case NetworkBadgeMode::EspNow:
            return isLinkOnline() ? kColorEspNow : kColorWarn;
        case NetworkBadgeMode::Off:
        default:
            return blendColor565(kColorPanelHi, kColorMuted, 0.28f);
    }
}

void drawNetworkGlyph(RemoteCanvas& canvas,
                      int16_t cx,
                      int16_t cy,
                      int16_t size,
                      NetworkBadgeMode mode,
                      uint16_t color) {
    if(mode == NetworkBadgeMode::EspNow) {
        const int16_t r = clampI16(static_cast<int16_t>(size / 5), 2, 4);
        const int16_t gap = clampI16(static_cast<int16_t>(size / 3), 4, 7);
        canvas.drawLine(static_cast<int16_t>(cx - gap),
                        cy,
                        static_cast<int16_t>(cx + gap),
                        cy,
                        color);
        canvas.fillCircle(static_cast<int16_t>(cx - gap), cy, r, color);
        canvas.fillCircle(static_cast<int16_t>(cx + gap), cy, r, color);
        return;
    }

    const int16_t half = static_cast<int16_t>(size / 2);
    const int16_t baseY = static_cast<int16_t>(cy + half / 3);
    canvas.fillCircle(cx, baseY, clampI16(static_cast<int16_t>(size / 10), 1, 3), color);
    canvas.drawLine(static_cast<int16_t>(cx - half / 2),
                    static_cast<int16_t>(baseY - half / 3),
                    cx,
                    static_cast<int16_t>(baseY - half / 2),
                    color);
    canvas.drawLine(cx,
                    static_cast<int16_t>(baseY - half / 2),
                    static_cast<int16_t>(cx + half / 2),
                    static_cast<int16_t>(baseY - half / 3),
                    color);
    canvas.drawLine(static_cast<int16_t>(cx - half),
                    static_cast<int16_t>(baseY - half),
                    cx,
                    static_cast<int16_t>(baseY - half - half / 4),
                    color);
    canvas.drawLine(cx,
                    static_cast<int16_t>(baseY - half - half / 4),
                    static_cast<int16_t>(cx + half),
                    static_cast<int16_t>(baseY - half),
                    color);

    if(mode == NetworkBadgeMode::WifiAp) {
        canvas.drawLine(static_cast<int16_t>(cx - half / 2),
                        static_cast<int16_t>(baseY + half / 2),
                        static_cast<int16_t>(cx + half / 2),
                        static_cast<int16_t>(baseY + half / 2),
                        color);
    } else if(mode == NetworkBadgeMode::Off) {
        canvas.drawLine(static_cast<int16_t>(cx - half),
                        static_cast<int16_t>(cy + half),
                        static_cast<int16_t>(cx + half),
                        static_cast<int16_t>(cy - half),
                        color);
    }
}

Rect networkIndicatorRect(const Layout& layout) {
    if(useRoundNavigation()) {
        return {
            layout.topLeft.x,
            layout.topLeft.y,
            static_cast<int16_t>(layout.topRight.x + layout.topRight.w - layout.topLeft.x),
            layout.topLeft.h
        };
    }
    return layout.topLeft;
}

void drawNetworkPill(RemoteCanvas& canvas, const Rect& rect, uint16_t bgColor) {
    const NetworkBadgeMode mode = currentNetworkBadgeMode();
    const String label = networkBadgeLabel(mode);
    const uint16_t fill = networkBadgeFill(mode);
    const uint16_t textColor = mode == NetworkBadgeMode::Off ? kColorText : kColorBlack;
    const int16_t radius = rect.h / 2;
    const bool round = useRoundNavigation();
    const uint16_t face = round ? blendColor565(kColorPanel, fill, 0.34f) : fill;
    const uint16_t border = round ? blendColor565(face, bgColor, 0.08f) : kColorPanelHi;

    if(round) {
        drawNestCard(canvas, rect, face, border, true);
    } else {
        canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, face);
        canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, border);
    }

    const bool textOnly = round || rect.w < 124;
    if(textOnly) {
        const RemoteTextStyle style = chooseLabelTextStyle(label, static_cast<int16_t>(rect.w - 12));
        canvas.drawCenteredText(fitTextToWidth(label, static_cast<int16_t>(rect.w - 12), style),
                                static_cast<int16_t>(rect.x + rect.w / 2),
                                static_cast<int16_t>(rect.y + rect.h / 2),
                                style,
                                textColor,
                                face);
        return;
    }

    const int16_t iconSize = clampI16(static_cast<int16_t>(rect.h - 10), 12, 18);
    const int16_t iconX = static_cast<int16_t>(rect.x + clampI16(static_cast<int16_t>(rect.h / 2), 10, 15));
    const int16_t centerY = static_cast<int16_t>(rect.y + rect.h / 2);
    const int16_t labelLeft = static_cast<int16_t>(iconX + iconSize / 2 + 5);
    const int16_t labelW = max<int16_t>(20, static_cast<int16_t>(rect.x + rect.w - labelLeft - 6));
    const RemoteTextStyle style =
        round ? chooseLabelTextStyle(label, labelW) : pillTextStyle();

    drawNetworkGlyph(canvas,
                     iconX,
                     centerY,
                     iconSize,
                     mode,
                     mode == NetworkBadgeMode::Off ? kColorMuted : kColorBlack);
    canvas.drawCenteredText(fitTextToWidth(label, labelW, style),
                            static_cast<int16_t>(labelLeft + labelW / 2),
                            centerY,
                            style,
                            textColor,
                            face);
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

int16_t batteryLevelPercent(const RemoteBatteryStatus& battery) {
    if(battery.percent >= 0) {
        return clampI16(static_cast<int16_t>(battery.percent), 0, 100);
    }
    if(battery.millivolts > 0) {
        return clampI16(static_cast<int16_t>(((battery.millivolts - 3300) * 100) / 900), 0, 100);
    }
    return 0;
}

String compactVoltageNumber(int32_t millivolts) {
    if(millivolts <= 0) {
        return "";
    }

    int32_t decivolts = (millivolts + 50) / 100;
    const int32_t whole = decivolts / 10;
    const int32_t frac = abs(decivolts % 10);
    String out;
    out.reserve(4);
    out += String(whole);
    out += '.';
    out += String(frac);
    return out;
}

String batteryIndicatorNumber(const RemoteBatteryStatus& battery) {
    if(!battery.supported) {
        return "";
    }
    if(battery.batteryPresent && battery.percent >= 0) {
        return String(static_cast<int>(battery.percent)) + "%";
    }
    if(battery.usbPresent) {
        return "";
    }
    if(battery.batteryPresent && battery.millivolts > 0) {
        return compactVoltageNumber(battery.millivolts);
    }
    if(battery.systemMillivolts > 0) {
        return compactVoltageNumber(battery.systemMillivolts);
    }
    return "";
}

uint16_t batteryIndicatorAccent(const RemoteBatteryStatus& battery) {
    return batteryStateAccent(battery, kColorMuted);
}

Rect batteryIndicatorRect(const Layout& layout) {
    const int16_t height = useRoundNavigation()
                               ? clampI16(static_cast<int16_t>(layout.height / 5), 84, 96)
                               : clampI16(static_cast<int16_t>(layout.topPillH + 22), 46, 58);
    const int16_t width = useRoundNavigation()
                              ? clampI16(static_cast<int16_t>(layout.width / 18), 24, 28)
                              : max<int16_t>(layout.topRight.w, 58);
    if(!useRoundNavigation()) {
        return {
            static_cast<int16_t>(layout.topRight.x + layout.topRight.w - width),
            layout.topRight.y,
            width,
            height
        };
    }

    const int16_t edgeInset = clampI16(static_cast<int16_t>(layout.pad / 2), 6, 10);
    const int16_t x = static_cast<int16_t>(layout.width - edgeInset - width);
    const int16_t y = static_cast<int16_t>((layout.height - height) / 2);
    return {x, y, width, height};
}

void drawBoltIcon(RemoteCanvas& canvas, int16_t cx, int16_t cy, int16_t size, uint16_t color) {
    const int16_t half = static_cast<int16_t>(size / 2);
    canvas.drawLine(static_cast<int16_t>(cx + half / 2),
                    static_cast<int16_t>(cy - half),
                    static_cast<int16_t>(cx - half / 2),
                    cy,
                    color);
    canvas.drawLine(static_cast<int16_t>(cx - half / 2),
                    cy,
                    static_cast<int16_t>(cx + half / 3),
                    cy,
                    color);
    canvas.drawLine(static_cast<int16_t>(cx + half / 3),
                    cy,
                    static_cast<int16_t>(cx - half / 2),
                    static_cast<int16_t>(cy + half),
                    color);
}

void drawUsbPowerIcon(RemoteCanvas& canvas, const Rect& icon, uint16_t accent, uint16_t face, uint16_t markColor) {
    const int16_t cx = static_cast<int16_t>(icon.x + icon.w / 2);
    const int16_t cy = static_cast<int16_t>(icon.y + icon.h / 2);
    const int16_t radius = clampI16(static_cast<int16_t>(min<int16_t>(icon.w, icon.h) / 3), 5, 8);
    canvas.drawCircle(cx, cy, radius, accent);
    canvas.drawCircle(cx, cy, static_cast<int16_t>(radius + 1), blendColor565(accent, markColor, 0.18f));
    canvas.drawLine(cx, static_cast<int16_t>(cy - radius - 2), cx, cy, markColor);
    canvas.drawLine(static_cast<int16_t>(cx - radius + 1),
                    static_cast<int16_t>(cy - radius - 1),
                    static_cast<int16_t>(cx + radius - 1),
                    static_cast<int16_t>(cy - radius - 1),
                    blendColor565(face, markColor, 0.38f));
}

void drawUsbCIcon(RemoteCanvas& canvas, const Rect& icon, uint16_t accent, uint16_t face, uint16_t markColor) {
    const int16_t bodyH = clampI16(static_cast<int16_t>(icon.h / 2), 6, 10);
    const int16_t bodyY = static_cast<int16_t>(icon.y + (icon.h - bodyH) / 2);
    const int16_t radius = clampI16(static_cast<int16_t>(bodyH / 2), 3, 5);
    const uint16_t shell = blendColor565(face, accent, 0.28f);
    canvas.fillRoundRect(icon.x, bodyY, icon.w, bodyH, radius, shell);
    canvas.drawRoundRect(icon.x, bodyY, icon.w, bodyH, radius, blendColor565(markColor, accent, 0.30f));
    if(icon.w > 14) {
        const int16_t slotY = static_cast<int16_t>(bodyY + bodyH / 2);
        canvas.drawLine(static_cast<int16_t>(icon.x + 5),
                        slotY,
                        static_cast<int16_t>(icon.x + icon.w - 5),
                        slotY,
                        blendColor565(markColor, face, 0.35f));
    }
    canvas.drawLine(static_cast<int16_t>(icon.x + icon.w / 2),
                    static_cast<int16_t>(bodyY + bodyH),
                    static_cast<int16_t>(icon.x + icon.w / 2),
                    static_cast<int16_t>(icon.y + icon.h),
                    accent);
}

void drawBatteryGlyph(RemoteCanvas& canvas,
                      const Rect& icon,
                      uint16_t accent,
                      uint16_t fillColor,
                      uint16_t face,
                      bool charging,
                      bool usbPresent,
                      int16_t level) {
    (void)charging;
    (void)usbPresent;
    const int16_t bodyW = clampI16(static_cast<int16_t>(icon.w - 8), 8, 11);
    const int16_t bodyH = clampI16(static_cast<int16_t>(icon.h - 4), 44, 62);
    const int16_t bodyX = static_cast<int16_t>(icon.x + (icon.w - bodyW) / 2);
    const int16_t bodyY = static_cast<int16_t>(icon.y + icon.h - bodyH);
    const int16_t capW = clampI16(static_cast<int16_t>(bodyW - 4), 4, 7);
    const int16_t capH = 3;
    const uint16_t outline = blendColor565(face, accent, 0.72f);
    const uint16_t empty = blendColor565(face, kColorBlack, 0.10f);

    canvas.fillRoundRect(bodyX, bodyY, bodyW, bodyH, 4, empty);
    canvas.drawRoundRect(bodyX, bodyY, bodyW, bodyH, 4, outline);
    canvas.fillRoundRect(static_cast<int16_t>(bodyX + (bodyW - capW) / 2),
                         static_cast<int16_t>(bodyY - capH),
                         capW,
                         capH,
                         2,
                         outline);

    const int16_t fillMax = static_cast<int16_t>(bodyH - 4);
    const int16_t fillH = clampI16(static_cast<int16_t>((fillMax * clampI16(level, 0, 100)) / 100), 0, fillMax);
    if(fillH > 0) {
        canvas.fillRoundRect(static_cast<int16_t>(bodyX + 1),
                             static_cast<int16_t>(bodyY + bodyH - 2 - fillH),
                             static_cast<int16_t>(bodyW - 2),
                             fillH,
                             2,
                             fillColor);
    }
}

void drawBatteryIndicator(RemoteCanvas& canvas, const Layout& layout, uint16_t backgroundColor) {
    const RemoteBatteryStatus& battery = app.battery;
    const Rect rect = batteryIndicatorRect(layout);
    if(rect.w <= 0 || rect.h <= 0) {
        return;
    }

    const uint16_t rawAccent = batteryIndicatorAccent(battery);
    const uint16_t accent = battery.supported ? blendColor565(rawAccent, kColorText, 0.08f) : kColorMuted;
    const uint16_t face = blendColor565(backgroundColor, kColorBlack, 0.34f);
    const uint16_t badgeFace = blendColor565(kColorPanel, backgroundColor, 0.44f);
    const uint16_t border = battery.supported ? blendColor565(rawAccent, kColorText, 0.18f)
                                              : blendColor565(kColorPanelHi, kColorText, 0.18f);
    const uint16_t statusAccent = battery.supported ? batteryStateAccent(battery, kColorMuted) : kColorMuted;
    const uint16_t statusFace = battery.supported ? blendColor565(badgeFace, statusAccent, 0.18f) : badgeFace;
    const uint16_t statusBorder = battery.supported ? blendColor565(statusAccent, kColorText, 0.22f) : border;
    const uint16_t markColor = battery.supported ? blendColor565(statusAccent, kColorText, 0.18f) : kColorText;
    const uint16_t glyphMarkColor = battery.supported ? accent : kColorText;
    const uint16_t numberText = battery.supported ? blendColor565(rawAccent, kColorText, 0.18f) : kColorText;
    const String number = batteryIndicatorNumber(battery);
    const int16_t centerX = static_cast<int16_t>(rect.x + rect.w / 2);
    const Rect statusBadge = {
        static_cast<int16_t>(centerX - 8),
        rect.y,
        16,
        16
    };
    canvas.fillRoundRect(statusBadge.x, statusBadge.y, statusBadge.w, statusBadge.h, 8, statusFace);
    canvas.drawRoundRect(statusBadge.x, statusBadge.y, statusBadge.w, statusBadge.h, 8, statusBorder);
    const int16_t statusY = static_cast<int16_t>(statusBadge.y + statusBadge.h / 2);
    if(battery.charging) {
        drawBoltIcon(canvas, centerX, statusY, 12, markColor);
    } else if(battery.usbPresent) {
        const Rect powerIcon = {
            static_cast<int16_t>(centerX - 6),
            static_cast<int16_t>(statusY - 6),
            12,
            12
        };
        if(batteryUsbPowerHold(battery)) {
            drawUsbPowerIcon(canvas, powerIcon, statusAccent, statusFace, markColor);
        } else {
            drawUsbCIcon(canvas, powerIcon, kColorAccent, statusFace, markColor);
        }
    } else if(battery.supported) {
        canvas.fillCircle(centerX, statusY, 2, markColor);
    }

    const int16_t numberH = useRoundNavigation() ? 20 : 16;
    const int16_t numberWOuter = useRoundNavigation()
                                     ? clampI16(static_cast<int16_t>(rect.w + 26), 48, 56)
                                     : rect.w;
    const Rect numberBadge = {
        static_cast<int16_t>(centerX - numberWOuter / 2),
        static_cast<int16_t>(rect.y + rect.h - numberH),
        numberWOuter,
        numberH
    };
    const int16_t iconW = clampI16(static_cast<int16_t>(rect.w - 10), 14, 18);
    const int16_t iconY = static_cast<int16_t>(statusBadge.y + statusBadge.h + 6);
    const int16_t iconH = clampI16(static_cast<int16_t>(numberBadge.y - iconY - 5), 48, 62);
    const int16_t iconX = static_cast<int16_t>(centerX - iconW / 2);
    const Rect icon = {
        iconX,
        iconY,
        iconW,
        iconH
    };
    const Rect iconSticker = {
        static_cast<int16_t>(centerX - 8),
        static_cast<int16_t>(icon.y - 3),
        16,
        static_cast<int16_t>(icon.h + 6)
    };
    canvas.fillRoundRect(iconSticker.x, iconSticker.y, iconSticker.w, iconSticker.h, 8, face);
    canvas.drawRoundRect(iconSticker.x, iconSticker.y, iconSticker.w, iconSticker.h, 8, border);

    drawBatteryGlyph(canvas,
                     icon,
                     accent,
                     rawAccent,
                     face,
                     battery.charging,
                     battery.usbPresent,
                     batteryLevelPercent(battery));

    if(number.length() > 0) {
        const int16_t numberW = numberBadge.w;
        const RemoteTextStyle numberStyle =
            textFitsWidth(number, numberW, RemoteTextStyle::Small, 0) ? RemoteTextStyle::Small : RemoteTextStyle::Tiny;
        const String numberTextFit = fitTextToWidth(number, numberW, numberStyle, 0);
        const int16_t numberY = static_cast<int16_t>(numberBadge.y + numberBadge.h / 2);
        canvas.drawCenteredText(numberTextFit,
                                centerX,
                                static_cast<int16_t>(numberY + 1),
                                numberStyle,
                                kColorBlack,
                                backgroundColor);
        canvas.drawCenteredText(numberTextFit,
                                static_cast<int16_t>(centerX - 1),
                                numberY,
                                numberStyle,
                                kColorBlack,
                                backgroundColor);
        canvas.drawCenteredText(numberTextFit,
                                static_cast<int16_t>(centerX + 1),
                                numberY,
                                numberStyle,
                                kColorBlack,
                                backgroundColor);
        canvas.drawCenteredText(numberTextFit,
                                centerX,
                                numberY,
                                numberStyle,
                                numberText,
                                backgroundColor);
    } else if(battery.usbPresent) {
        const Rect usbBadgeIcon = {
            static_cast<int16_t>(numberBadge.x + 10),
            static_cast<int16_t>(numberBadge.y + 2),
            static_cast<int16_t>(numberBadge.w - 20),
            static_cast<int16_t>(numberBadge.h - 3)
        };
        drawUsbCIcon(canvas, usbBadgeIcon, accent, face, markColor);
    }
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

enum class NetworkCurveKind : uint8_t {
    Toast,
    WifiLan,
    WifiAp,
    WifiJoin,
    WifiSetup,
    WifiSynced,
    WifiFallback,
    EspNowLink,
    EspNowSearch,
    EspNowOffline,
    Off,
};

struct NetworkCurveStyle {
    uint16_t accent = kColorMuted;
    uint16_t text = kColorMuted;
    bool subtle = true;
};

NetworkCurveKind networkCurveKindForLabel(const String& label) {
    const String text = upperText(label);
    if(text.startsWith("WIFI LAN")) {
        return NetworkCurveKind::WifiLan;
    }
    if(text.startsWith("WIFI AP")) {
        return NetworkCurveKind::WifiAp;
    }
    if(text.startsWith("WIFI JOIN") || text.startsWith("WIFI START")) {
        return NetworkCurveKind::WifiJoin;
    }
    if(text.startsWith("WIFI SETUP")) {
        return NetworkCurveKind::WifiSetup;
    }
    if(text.startsWith("WIFI SYNCED")) {
        return NetworkCurveKind::WifiSynced;
    }
    if(text.startsWith("WIFI FALLBACK")) {
        return NetworkCurveKind::WifiFallback;
    }
    if(text.startsWith("ESP-NOW LINK")) {
        return NetworkCurveKind::EspNowLink;
    }
    if(text.startsWith("ESP-NOW SEARCH")) {
        return NetworkCurveKind::EspNowSearch;
    }
    if(text.startsWith("ESP-NOW OFFLINE")) {
        return NetworkCurveKind::EspNowOffline;
    }
    if(text.startsWith("NETWORK OFF")) {
        return NetworkCurveKind::Off;
    }
    return NetworkCurveKind::Toast;
}

NetworkCurveStyle networkCurveStyleForLabel(const String& label, const HomePalette& palette) {
    const NetworkCurveKind kind = networkCurveKindForLabel(label);
    NetworkCurveStyle style;
    switch(kind) {
        case NetworkCurveKind::WifiLan:
            style.accent = blendColor565(kColorGood, palette.accent, 0.26f);
            style.subtle = false;
            break;
        case NetworkCurveKind::WifiAp:
            style.accent = blendColor565(kColorAccent, palette.accent, 0.20f);
            style.subtle = false;
            break;
        case NetworkCurveKind::WifiJoin:
            style.accent = blendColor565(kColorAccent, palette.accent, 0.36f);
            style.subtle = false;
            break;
        case NetworkCurveKind::WifiSetup:
            style.accent = blendColor565(kColorAccent, kColorMuted, 0.18f);
            style.subtle = false;
            break;
        case NetworkCurveKind::WifiSynced:
            style.accent = blendColor565(kColorGood, palette.accent, 0.18f);
            style.subtle = false;
            break;
        case NetworkCurveKind::WifiFallback:
            style.accent = blendColor565(kColorWarn, palette.accent, 0.34f);
            style.subtle = false;
            break;
        case NetworkCurveKind::EspNowLink:
            style.accent = blendColor565(kColorEspNow, palette.accent, 0.12f);
            style.subtle = false;
            break;
        case NetworkCurveKind::EspNowSearch:
            style.accent = blendColor565(kColorEspNow, palette.muted, 0.42f);
            style.subtle = true;
            break;
        case NetworkCurveKind::EspNowOffline:
            style.accent = blendColor565(kColorDanger, kColorEspNow, 0.36f);
            style.subtle = true;
            break;
        case NetworkCurveKind::Off:
            style.accent = blendColor565(kColorMuted, palette.background, 0.18f);
            style.subtle = true;
            break;
        case NetworkCurveKind::Toast:
        default:
            style.accent = blendColor565(palette.muted, palette.accent, 0.30f);
            style.subtle = false;
            break;
    }

    style.text = blendColor565(style.accent, kColorText, style.subtle ? 0.08f : 0.12f);
    return style;
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

    NetworkCurveStyle curveStyle = networkCurveStyleForLabel(bottomSource, palette);
    if(running && curveStyle.subtle) {
        curveStyle.text = blendColor565(curveStyle.text, palette.background, 0.08f);
    }
    drawArcText(canvas,
                bottomLabel,
                layout.homeDialCx,
                layout.homeDialCy,
                bottomRadius,
                90.0f,
                bottomSpan,
                true,
                bottomStyle,
                curveStyle.text,
                palette.background,
                curveStyle.accent,
                curveStyle.subtle);
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
    if(isRadioModeItem(item)) {
        return normalizedRadioModeLevel(value);
    }
    if(usesChoiceEditor(item)) {
        return value != 0 ? 1.0f : 0.0f;
    }
    return normalizedRangeValue(displayValueForItem(item, value), item.minValue, item.maxValue);
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
        if(isGaugeSetItem(item) && !gaugeSetMenuValueKnown()) {
            return blendColor565(kColorMuted, kColorPanelHi, 0.25f);
        }
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
        drawBatteryIndicator(canvas, layout, kColorBg);
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
    drawBatteryIndicator(canvas, layout, kColorBg);

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
                             fitTextToWidth(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4),
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
        drawBatteryIndicator(canvas, layout, kColorBg);

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
            const int16_t rowH = focused ? clampI16(static_cast<int16_t>(layout.rowH + 2), 48, 64)
                                         : clampI16(static_cast<int16_t>(42.0f + prominence * 16.0f), 38, 56);
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

            const String fittedLabel = focused
                                           ? fitTextToWidth(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4)
                                           : fitTextToWidthNoTilde(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4);
            drawSoftCenteredText(canvas,
                                 fittedLabel,
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
        const int16_t rowH = clampI16(static_cast<int16_t>(42.0f + prominence * 18.0f), 38, 62);
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
        const String fittedLabel = focused
                                       ? fitTextToWidth(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4)
                                       : fitTextToWidthNoTilde(label, static_cast<int16_t>(rowRect.w - 24), labelStyle, 4);
        drawSoftCenteredText(canvas,
                             fittedLabel,
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
        const int32_t displayLiveRpm = rpmMagnitude(liveRpm);
        const bool reverseLiveRpm = reverseRpmDisplayActive();
        const String targetValue = isGbwState() ? weightValueText(app.weightMg) : String(app.manualRpm);
        const String liveValue = isGbwState() ? weightValueText(app.status.currentWeightMg) : String(displayLiveRpm);
        const String primaryValue = showLiveValue ? liveValue : targetValue;
        const String unitText = isGbwState() ? "GRAMS" : "RPM";
        const String unitLine = (isRunningState() && app.hasStatus)
                                    ? unitText + "  TORQ " + String(app.status.torquePercent) + "%"
                                    : unitText;
        const int16_t valueWidth = static_cast<int16_t>((layout.homeDialInnerRadius * 2) + 72);
        const int16_t unitWidth = static_cast<int16_t>((layout.homeDialInnerRadius * 2) + 82);
        const RemoteTextStyle valueStyle = chooseValueTextStyle(primaryValue, valueWidth);
        const RemoteTextStyle unitStyle = chooseLabelTextStyle(unitLine, unitWidth);
        const String gapLine = homeGapValueLabel();
        const RemoteTextStyle gapValueStyle = isLargeCanvas() ? RemoteTextStyle::Large : RemoteTextStyle::Medium;
        const RemoteTextStyle gapUnitStyle = isLargeCanvas() ? RemoteTextStyle::Medium : RemoteTextStyle::Small;
        const uint16_t ringAccent = reverseLiveRpm ? reverseRpmAccentColor() : palette.accent;
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
        canvas.drawCenteredText(fitTextToWidth(unitLine,
                                               unitWidth,
                                               unitStyle,
                                               12),
                                layout.homeDialCx,
                                static_cast<int16_t>(layout.homeDialCy + 34),
                                unitStyle,
                                unitColor,
                                palette.background);
        if(kShowBurrGapReadout) {
            drawGapReadout(canvas,
                           layout.homeDialCx,
                           static_cast<int16_t>(layout.homeDialCy + (isLargeCanvas() ? 72 : 54)),
                           gapLine,
                           gapValueStyle,
                           gapUnitStyle,
                           kColorGauge,
                           palette.background);
        }

        const int16_t connectionY = static_cast<int16_t>(layout.homeDialOuterRadius * 0.69f);
        const int16_t connectionW = static_cast<int16_t>(layout.homeDialOuterRadius * 1.30f);
        const String connectionLabel = String(isLargeCanvas() ? "CTRL " : "") +
                                       (isLinkOnline() ? "ONLINE" : "OFFLINE");
        canvas.drawCenteredText(fitTextToWidth(connectionLabel, connectionW, RemoteTextStyle::Small, 0),
                                layout.homeDialCx,
                                static_cast<int16_t>(layout.homeDialCy + connectionY),
                                RemoteTextStyle::Small,
                                linkStatusColor(),
                                palette.panel);
        drawBatteryIndicator(canvas, layout, palette.background);
        return;
    }

    drawPill(canvas,
             layout.topCenter,
             app.hasStatus ? app.status.drive : "?",
             palette.panel,
             kColorText);
    drawBatteryIndicator(canvas, layout, palette.background);

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

    const bool reverseLiveRpm = reverseRpmDisplayActive();
    const uint16_t liveValueColor = reverseLiveRpm ? blendColor565(kColorText, kColorDanger, 0.72f) : kColorText;

    canvas.drawCenteredText(isGbwState() ? weightLabel(app.weightMg) : String(app.manualRpm),
                            layout.homeDialCx,
                            static_cast<int16_t>(layout.homeDialCy - (isLargeCanvas() ? 22 : 12)),
                            heroTextStyle(),
                            liveValueColor,
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

    const String stateLine = isLinkOnline() ? homePrimaryStatusLine() : controllerConnectionText();
    const String telemetryLine = isLinkOnline()
        ? String("RPM ") + (app.hasStatus ? String(rpmMagnitude(homeWheelDisplayRpm())) : "-") +
          "  WT " + (app.hasStatus ? weightLabel(app.status.currentWeightMg) : String("-"))
        : networkCurveStatusText();
    const String gapLine = homeGapValueLabel();

    canvas.drawCenteredText(stateLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + 10),
                            bodyTextStyle(),
                            linkStatusColor(),
                            palette.background);
    canvas.drawCenteredText(telemetryLine,
                            layout.width / 2,
                            static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + (isLargeCanvas() ? 38 : 22)),
                            tinyTextStyle(),
                            palette.muted,
                            palette.background);
    if(kShowBurrGapReadout) {
        drawGapReadout(canvas,
                       layout.width / 2,
                       static_cast<int16_t>(layout.homeActionButton.y + layout.homeActionButton.h + layout.gap + (isLargeCanvas() ? 58 : 36)),
                       gapLine,
                       bodyTextStyle(),
                       tinyTextStyle(),
                       kColorGauge,
                       palette.background);
    }

    drawTabs(canvas, layout);
}

int16_t pressureGraphY(const Rect& graph, uint16_t centiBar) {
    const uint16_t displayValue = min<uint16_t>(centiBar, remote_config::kPressureProfilerGraphMaxCentiBar);
    const int32_t offset =
        (static_cast<int32_t>(displayValue) * static_cast<int32_t>(graph.h)) /
        static_cast<int32_t>(remote_config::kPressureProfilerGraphMaxCentiBar);
    return static_cast<int16_t>(graph.y + graph.h - offset);
}

int16_t pressureGraphX(const Rect& graph, uint32_t elapsedMs, uint32_t visibleStartMs, uint32_t visibleMs) {
    if(visibleMs == 0 || elapsedMs <= visibleStartMs) {
        return graph.x;
    }
    const uint32_t relative = min<uint32_t>(elapsedMs - visibleStartMs, visibleMs);
    return static_cast<int16_t>(graph.x +
                                (static_cast<uint64_t>(relative) * graph.w) / visibleMs);
}

float pressureRadarAngle(uint32_t elapsedMs, uint32_t visibleStartMs, uint32_t visibleMs) {
    const uint32_t relativeMs = elapsedMs <= visibleStartMs ? 0 : elapsedMs - visibleStartMs;
    const float phase = visibleMs == 0
                            ? 0.0f
                            : clampUnit(static_cast<float>(relativeMs) / static_cast<float>(visibleMs));
    return static_cast<float>(-PI / 2.0f) + phase * static_cast<float>(2.0f * PI);
}

void pressureRadarPoint(int16_t centerX,
                        int16_t centerY,
                        int16_t innerRadius,
                        int16_t outerRadius,
                        uint16_t centiBar,
                        uint32_t elapsedMs,
                        uint32_t visibleStartMs,
                        uint32_t visibleMs,
                        int16_t* outX,
                        int16_t* outY) {
    const uint16_t displayValue = min<uint16_t>(centiBar, remote_config::kPressureProfilerGraphMaxCentiBar);
    const int16_t radiusSpan = max<int16_t>(static_cast<int16_t>(outerRadius - innerRadius), 1);
    const int16_t radius = static_cast<int16_t>(
        innerRadius +
        (static_cast<int32_t>(displayValue) * radiusSpan) /
            static_cast<int32_t>(remote_config::kPressureProfilerGraphMaxCentiBar));
    const float angle = pressureRadarAngle(elapsedMs, visibleStartMs, visibleMs);
    if(outX != nullptr) {
        *outX = static_cast<int16_t>(centerX + lroundf(cosf(angle) * radius));
    }
    if(outY != nullptr) {
        *outY = static_cast<int16_t>(centerY + lroundf(sinf(angle) * radius));
    }
}

void drawPressureRadarSweep(RemoteCanvas& canvas,
                            int16_t centerX,
                            int16_t centerY,
                            int16_t innerRadius,
                            int16_t outerRadius,
                            float angle,
                            uint16_t accent,
                            uint16_t gridColor) {
    for(uint8_t i = 0; i < 6; ++i) {
        const float tailAngle = angle - static_cast<float>(i) * 0.055f;
        const uint16_t color = blendColor565(gridColor, accent, 0.68f - static_cast<float>(i) * 0.10f);
        const int16_t x0 = static_cast<int16_t>(centerX + lroundf(cosf(tailAngle) * innerRadius));
        const int16_t y0 = static_cast<int16_t>(centerY + lroundf(sinf(tailAngle) * innerRadius));
        const int16_t x1 = static_cast<int16_t>(centerX + lroundf(cosf(tailAngle) * outerRadius));
        const int16_t y1 = static_cast<int16_t>(centerY + lroundf(sinf(tailAngle) * outerRadius));
        canvas.drawLine(x0, y0, x1, y1, color);
    }
}

void drawPressureProfilerRadar(RemoteCanvas& canvas,
                               const Layout& layout,
                               uint32_t visibleStartMs,
                               uint32_t visibleEndMs,
                               uint16_t bg,
                               uint16_t accent,
                               uint16_t gridColor) {
    const int16_t shortSide = min<int16_t>(layout.width, layout.height);
    const int16_t edge = useRoundNavigation() ? 10 : 16;
    const int16_t centerX = static_cast<int16_t>(layout.width / 2);
    const int16_t centerY = static_cast<int16_t>(layout.height / 2 + (useRoundNavigation() ? 4 : 8));
    const int16_t outerRadius = clampI16(static_cast<int16_t>(shortSide / 2 - edge),
                                         86,
                                         static_cast<int16_t>(shortSide / 2 - 4));
    const int16_t innerRadius = clampI16(static_cast<int16_t>(outerRadius * 0.16f), 18, 34);
    const uint16_t softGrid = blendColor565(gridColor, bg, 0.42f);
    const uint16_t majorGrid = blendColor565(gridColor, accent, 0.24f);
    const uint32_t visibleMs = max<uint32_t>(visibleEndMs - visibleStartMs, 1);

    canvas.fillCircle(centerX, centerY, outerRadius, blendColor565(bg, packColor565(0, 42, 28), 0.28f));
    canvas.drawCircle(centerX, centerY, outerRadius, majorGrid);
    canvas.drawCircle(centerX, centerY, static_cast<int16_t>(outerRadius - 1), blendColor565(majorGrid, kColorText, 0.10f));

    constexpr uint8_t kRingBars[] = {4, 8, 12, 16};
    for(uint8_t i = 0; i < sizeof(kRingBars); ++i) {
        const uint16_t centiBar = static_cast<uint16_t>(kRingBars[i]) * 100;
        const int16_t ringRadius = static_cast<int16_t>(
            innerRadius +
            (static_cast<int32_t>(centiBar) * (outerRadius - innerRadius)) /
                static_cast<int32_t>(remote_config::kPressureProfilerGraphMaxCentiBar));
        const uint16_t ringColor = i == (sizeof(kRingBars) - 1) ? majorGrid : softGrid;
        canvas.drawCircle(centerX, centerY, ringRadius, ringColor);
        canvas.drawTextTopRight(String(static_cast<int>(kRingBars[i])),
                                static_cast<int16_t>(centerX + ringRadius - 5),
                                static_cast<int16_t>(centerY - 8),
                                RemoteTextStyle::Tiny,
                                blendColor565(kColorMuted, accent, 0.18f),
                                bg);
    }

    for(uint8_t i = 0; i < 12; ++i) {
        const float angle = static_cast<float>(-PI / 2.0f) + static_cast<float>(i) * static_cast<float>(PI / 6.0f);
        const int16_t x0 = static_cast<int16_t>(centerX + lroundf(cosf(angle) * innerRadius));
        const int16_t y0 = static_cast<int16_t>(centerY + lroundf(sinf(angle) * innerRadius));
        const int16_t x1 = static_cast<int16_t>(centerX + lroundf(cosf(angle) * outerRadius));
        const int16_t y1 = static_cast<int16_t>(centerY + lroundf(sinf(angle) * outerRadius));
        canvas.drawLine(x0, y0, x1, y1, (i % 3) == 0 ? majorGrid : softGrid);
    }

    const int16_t senseRadius = static_cast<int16_t>(
        innerRadius +
        (static_cast<int32_t>(remote_config::kPressureProfilerSenseCentiBar) * (outerRadius - innerRadius)) /
            static_cast<int32_t>(remote_config::kPressureProfilerGraphMaxCentiBar));
    canvas.drawCircle(centerX, centerY, senseRadius, blendColor565(kColorAccent2, bg, 0.22f));

    bool hasPrev = false;
    int16_t prevX = 0;
    int16_t prevY = 0;
    for(int age = static_cast<int>(g_pressureProfiler.sampleCount) - 1; age >= 0; --age) {
        uint16_t centiBar = 0;
        uint32_t elapsed = 0;
        if(!pressureProfilerSampleAt(static_cast<uint16_t>(age), &centiBar, &elapsed)) {
            continue;
        }
        if(elapsed < visibleStartMs || elapsed > visibleEndMs) {
            continue;
        }

        int16_t x = centerX;
        int16_t y = centerY;
        pressureRadarPoint(centerX,
                           centerY,
                           innerRadius,
                           outerRadius,
                           centiBar,
                           elapsed,
                           visibleStartMs,
                           visibleMs,
                           &x,
                           &y);
        const float freshness = g_pressureProfiler.sampleCount <= 1
                                    ? 1.0f
                                    : clampUnit(static_cast<float>(g_pressureProfiler.sampleCount - 1 - age) /
                                                static_cast<float>(g_pressureProfiler.sampleCount - 1));
        const uint16_t lineColor = blendColor565(blendColor565(gridColor, bg, 0.32f), accent, 0.28f + freshness * 0.58f);
        if(hasPrev) {
            canvas.drawLine(prevX, prevY, x, y, blendColor565(lineColor, kColorText, 0.12f));
            canvas.drawLine(prevX, static_cast<int16_t>(prevY + 1), x, static_cast<int16_t>(y + 1), lineColor);
        } else {
            canvas.fillCircle(x, y, 2, lineColor);
        }
        prevX = x;
        prevY = y;
        hasPrev = true;
    }

    const uint32_t sweepElapsed = g_pressureProfiler.running
                                      ? min<uint32_t>(millis() - g_pressureProfiler.startedAt, visibleEndMs)
                                      : visibleStartMs + ((millis() - g_pressureProfiler.armedAt) % visibleMs);
    const float sweepAngle = pressureRadarAngle(sweepElapsed, visibleStartMs, visibleMs);
    drawPressureRadarSweep(canvas, centerX, centerY, innerRadius, outerRadius, sweepAngle, accent, softGrid);

    uint32_t dotElapsed = sweepElapsed;
    if(g_pressureProfiler.running) {
        dotElapsed = min<uint32_t>(millis() - g_pressureProfiler.startedAt, visibleEndMs);
    }
    int16_t dotX = centerX;
    int16_t dotY = centerY;
    pressureRadarPoint(centerX,
                       centerY,
                       innerRadius,
                       outerRadius,
                       g_pressureProfiler.currentCentiBar,
                       dotElapsed,
                       visibleStartMs,
                       visibleMs,
                       &dotX,
                       &dotY);
    canvas.fillCircle(dotX, dotY, 4, blendColor565(accent, kColorText, 0.20f));
    canvas.drawCircle(dotX, dotY, 7, blendColor565(accent, bg, 0.10f));
    canvas.drawCircle(centerX, centerY, innerRadius, blendColor565(accent, kColorText, 0.16f));
}

void drawPressureProfilerGraph(RemoteCanvas& canvas,
                               const Rect& graph,
                               uint32_t visibleStartMs,
                               uint32_t visibleEndMs,
                               uint16_t traceColor,
                               uint16_t themeColor,
                               uint16_t panelColor,
                               uint16_t gridColor) {
    const uint16_t graphFace = blendColor565(panelColor, packColor565(0, 34, 24), 0.34f);
    const uint16_t minorGrid = blendColor565(graphFace, gridColor, 0.13f);
    const uint16_t majorGrid = blendColor565(graphFace, gridColor, 0.23f);
    const uint16_t traceHi = blendColor565(traceColor, packColor565(255, 120, 78), 0.20f);
    const uint16_t labelColor = blendColor565(themeColor, graphFace, 0.34f);
    const uint16_t axisColor = blendColor565(gridColor, themeColor, 0.30f);
    const uint16_t axisLabel = blendColor565(themeColor, graphFace, 0.24f);
    canvas.fillRect(graph.x, graph.y, graph.w, graph.h, graphFace);
    canvas.drawRect(graph.x, graph.y, graph.w, graph.h, blendColor565(majorGrid, graphFace, 0.16f));
    canvas.drawLine(graph.x, graph.y, graph.x, static_cast<int16_t>(graph.y + graph.h), axisColor);
    canvas.drawLine(static_cast<int16_t>(graph.x + 1),
                    graph.y,
                    static_cast<int16_t>(graph.x + 1),
                    static_cast<int16_t>(graph.y + graph.h),
                    blendColor565(axisColor, graphFace, 0.12f));
    canvas.drawLine(graph.x,
                    static_cast<int16_t>(graph.y + graph.h),
                    static_cast<int16_t>(graph.x + graph.w),
                    static_cast<int16_t>(graph.y + graph.h),
                    axisColor);
    canvas.drawLine(graph.x,
                    static_cast<int16_t>(graph.y + graph.h - 1),
                    static_cast<int16_t>(graph.x + graph.w),
                    static_cast<int16_t>(graph.y + graph.h - 1),
                    blendColor565(axisColor, graphFace, 0.12f));

    const int16_t meshCell = clampI16(static_cast<int16_t>(min<int16_t>(graph.w, graph.h) / 8), 24, 42);
    uint8_t meshIndex = 1;
    for(int16_t x = static_cast<int16_t>(graph.x + meshCell);
        x < static_cast<int16_t>(graph.x + graph.w);
        x = static_cast<int16_t>(x + meshCell), ++meshIndex) {
        const uint16_t color = (meshIndex % 2) == 0 ? majorGrid : minorGrid;
        canvas.drawLine(x, graph.y, x, static_cast<int16_t>(graph.y + graph.h), color);
    }
    meshIndex = 1;
    for(int16_t y = static_cast<int16_t>(graph.y + meshCell);
        y < static_cast<int16_t>(graph.y + graph.h);
        y = static_cast<int16_t>(y + meshCell), ++meshIndex) {
        const uint16_t color = (meshIndex % 2) == 0 ? majorGrid : minorGrid;
        canvas.drawLine(graph.x, y, static_cast<int16_t>(graph.x + graph.w), y, color);
    }

    constexpr uint8_t kYSteps[] = {0, 4, 8, 12, 16};
    canvas.drawTextTopRight("BAR",
                            static_cast<int16_t>(graph.x - 4),
                            static_cast<int16_t>(graph.y - 20),
                            RemoteTextStyle::Tiny,
                            labelColor,
                            panelColor);
    for(uint8_t i = 0; i < sizeof(kYSteps); ++i) {
        const uint16_t centiBar = static_cast<uint16_t>(kYSteps[i]) * 100;
        const int16_t y = pressureGraphY(graph, centiBar);
        canvas.drawLine(graph.x, y, static_cast<int16_t>(graph.x + graph.w), y, majorGrid);
        canvas.drawLine(static_cast<int16_t>(graph.x - 6), y, graph.x, y, axisColor);
        canvas.drawTextTopRight(String(static_cast<int>(kYSteps[i])),
                                static_cast<int16_t>(graph.x - 8),
                                static_cast<int16_t>(y - 7),
                                RemoteTextStyle::Tiny,
                                labelColor,
                                panelColor);
    }

    const uint32_t visibleMs = max<uint32_t>(visibleEndMs - visibleStartMs, 1);
    for(uint8_t i = 0; i <= 6; ++i) {
        const uint32_t tickElapsed = visibleStartMs + (visibleMs * i) / 6;
        const int16_t x = pressureGraphX(graph, tickElapsed, visibleStartMs, visibleMs);
        canvas.drawLine(x, graph.y, x, static_cast<int16_t>(graph.y + graph.h), i == 0 || i == 6 ? majorGrid : minorGrid);
        canvas.drawLine(x,
                        static_cast<int16_t>(graph.y + graph.h),
                        x,
                        static_cast<int16_t>(graph.y + graph.h + 6),
                        axisColor);
        if((i % 2) == 0) {
            const String tickLabel = pressureElapsedLabel(tickElapsed);
            const int16_t labelX = clampI16(x,
                                            static_cast<int16_t>(graph.x + 14),
                                            static_cast<int16_t>(graph.x + graph.w - 14));
            canvas.drawCenteredText(tickLabel,
                                    labelX,
                                    static_cast<int16_t>(graph.y + graph.h - 12),
                                    RemoteTextStyle::Tiny,
                                    axisLabel,
                                    graphFace);
        }
    }
    canvas.drawCenteredText(String("TIME ") + pressureElapsedLabel(visibleMs),
                            static_cast<int16_t>(graph.x + graph.w / 2),
                            static_cast<int16_t>(graph.y + graph.h + 16),
                            RemoteTextStyle::Tiny,
                            blendColor565(labelColor, axisColor, 0.18f),
                            panelColor);

    const int16_t senseY = pressureGraphY(graph, remote_config::kPressureProfilerSenseCentiBar);
    canvas.drawLine(graph.x,
                    senseY,
                    static_cast<int16_t>(graph.x + graph.w),
                    senseY,
                    blendColor565(graphFace, kColorAccent2, 0.28f));
    canvas.drawLine(graph.x, graph.y, graph.x, static_cast<int16_t>(graph.y + graph.h), axisColor);
    canvas.drawLine(static_cast<int16_t>(graph.x + 1),
                    graph.y,
                    static_cast<int16_t>(graph.x + 1),
                    static_cast<int16_t>(graph.y + graph.h),
                    blendColor565(axisColor, graphFace, 0.12f));
    canvas.drawLine(graph.x,
                    static_cast<int16_t>(graph.y + graph.h),
                    static_cast<int16_t>(graph.x + graph.w),
                    static_cast<int16_t>(graph.y + graph.h),
                    axisColor);
    canvas.drawLine(graph.x,
                    static_cast<int16_t>(graph.y + graph.h - 1),
                    static_cast<int16_t>(graph.x + graph.w),
                    static_cast<int16_t>(graph.y + graph.h - 1),
                    blendColor565(axisColor, graphFace, 0.12f));

    bool hasPrev = false;
    int16_t prevX = 0;
    int16_t prevY = 0;
    int16_t latestX = 0;
    int16_t latestY = 0;
    for(int age = static_cast<int>(g_pressureProfiler.sampleCount) - 1; age >= 0; --age) {
        uint16_t centiBar = 0;
        uint32_t elapsed = 0;
        if(!pressureProfilerSampleAt(static_cast<uint16_t>(age), &centiBar, &elapsed)) {
            continue;
        }
        if(elapsed < visibleStartMs || elapsed > visibleEndMs) {
            continue;
        }

        const int16_t x = pressureGraphX(graph, elapsed, visibleStartMs, visibleMs);
        const int16_t y = pressureGraphY(graph, centiBar);
        if(hasPrev) {
            canvas.drawLine(prevX, prevY, x, y, traceHi);
            canvas.drawLine(prevX, static_cast<int16_t>(prevY + 1), x, static_cast<int16_t>(y + 1), traceColor);
        } else {
            canvas.fillCircle(x, y, 2, traceColor);
        }
        prevX = x;
        prevY = y;
        latestX = x;
        latestY = y;
        hasPrev = true;
    }
    if(hasPrev) {
        canvas.fillCircle(latestX, latestY, 4, traceHi);
        canvas.drawCircle(latestX, latestY, 7, blendColor565(traceColor, graphFace, 0.12f));
    }
}

uint16_t dockWeatherAccent() {
    if(!app.dockWeather.valid) {
        return blendColor565(kColorMuted, kColorGood, 0.20f);
    }
    const uint16_t code = app.dockWeather.weatherCode;
    if(code == 0) {
        return packColor565(255, 210, 90);
    }
    if(code >= 95) {
        return blendColor565(kColorDanger, kColorAccent2, 0.28f);
    }
    if((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return packColor565(86, 190, 255);
    }
    if((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return blendColor565(kColorText, packColor565(130, 220, 255), 0.38f);
    }
    return blendColor565(kColorGood, kColorText, 0.24f);
}

String dockWeatherMetricLine() {
    if(app.dockWeather.valid) {
        String out;
        out.reserve(20);
        out += String(static_cast<int>(app.dockWeather.temperatureF));
        out += "F";
        if(app.dockWeather.precipitationPercent >= 0) {
            out += " ";
            out += String(static_cast<int>(app.dockWeather.precipitationPercent));
            out += "%";
        }
        return out;
    }
    return "";
}

String dockWeatherLine() {
    if(app.dockWeather.valid) {
        String out = dockWeatherMetricLine();
        if(out.length() > 0) {
            out += " ";
        }
        out += weatherCodeLabel(app.dockWeather.weatherCode);
        return out;
    }
    return "";
}

String dockLocationLine() {
    if(app.dockWeather.valid && app.dockWeather.location.length() > 0) {
        return app.dockWeather.location;
    }
    return remote_config::kDockClockLocationName;
}

void drawDockWeatherBackdrop(RemoteCanvas& canvas, const Layout& layout, uint16_t bg, uint16_t accent) {
    if(!app.dockWeather.valid) {
        return;
    }

    const int16_t cx = static_cast<int16_t>(layout.width / 2);
    const int16_t cy = static_cast<int16_t>(layout.height / 2);
    const uint16_t haze = blendColor565(bg, accent, 0.13f);
    const uint16_t hazeHi = blendColor565(bg, accent, 0.22f);
    const uint16_t hazeLow = blendColor565(bg, kColorBlack, 0.10f);

    if(app.dockWeather.valid && app.dockWeather.weatherCode == 0) {
        if(useRoundNavigation()) {
            // On the round clock this reads as a stray yellow arc on the bezel.
            return;
        }
        canvas.fillCircle(static_cast<int16_t>(cx - layout.width / 4),
                          static_cast<int16_t>(cy - layout.height / 4),
                          clampI16(static_cast<int16_t>(layout.width / 9), 28, 52),
                          hazeHi);
        for(uint8_t i = 0; i < 10; ++i) {
            const float angle = static_cast<float>(i) * static_cast<float>(2.0f * PI / 10.0f);
            const int16_t x0 = static_cast<int16_t>(cx - layout.width / 4 + lroundf(cosf(angle) * 42));
            const int16_t y0 = static_cast<int16_t>(cy - layout.height / 4 + lroundf(sinf(angle) * 42));
            const int16_t x1 = static_cast<int16_t>(cx - layout.width / 4 + lroundf(cosf(angle) * 76));
            const int16_t y1 = static_cast<int16_t>(cy - layout.height / 4 + lroundf(sinf(angle) * 76));
            canvas.drawLine(x0, y0, x1, y1, haze);
        }
    } else {
        const int16_t cloudY = static_cast<int16_t>(cy - layout.height / 5);
        canvas.fillCircle(static_cast<int16_t>(cx - 56), cloudY, 46, haze);
        canvas.fillCircle(static_cast<int16_t>(cx - 12), static_cast<int16_t>(cloudY - 18), 58, hazeHi);
        canvas.fillCircle(static_cast<int16_t>(cx + 46), static_cast<int16_t>(cloudY + 2), 42, haze);
        canvas.fillRoundRect(static_cast<int16_t>(cx - 102),
                             static_cast<int16_t>(cloudY + 14),
                             204,
                             38,
                             19,
                             haze);
    }

    if(app.dockWeather.valid &&
       ((app.dockWeather.weatherCode >= 51 && app.dockWeather.weatherCode <= 67) ||
        (app.dockWeather.weatherCode >= 80 && app.dockWeather.weatherCode <= 82) ||
        app.dockWeather.weatherCode >= 95)) {
        for(uint8_t i = 0; i < 10; ++i) {
            const int16_t x = static_cast<int16_t>((layout.width * (i + 1)) / 11);
            const int16_t y = static_cast<int16_t>(layout.height / 2 + ((i % 3) * 22));
            canvas.drawLine(x, y, static_cast<int16_t>(x - 10), static_cast<int16_t>(y + 28), hazeHi);
        }
    }

    if(!useRoundNavigation()) {
        const String weather = dockWeatherLine();
        canvas.drawCenteredText(fitTextToWidth(weather,
                                               static_cast<int16_t>(layout.width - layout.pad * 4),
                                               RemoteTextStyle::Large,
                                               0),
                                cx,
                                static_cast<int16_t>(layout.height - 54),
                                RemoteTextStyle::Large,
                                blendColor565(hazeLow, accent, 0.24f),
                                bg);
    }
}

void drawClockHand(RemoteCanvas& canvas,
                   int16_t cx,
                   int16_t cy,
                   int16_t radius,
                   float angle,
                   uint16_t color,
                   bool heavy) {
    const int16_t x = static_cast<int16_t>(cx + lroundf(cosf(angle) * radius));
    const int16_t y = static_cast<int16_t>(cy + lroundf(sinf(angle) * radius));
    canvas.drawLine(cx, cy, x, y, color);
    if(heavy) {
        canvas.drawLine(static_cast<int16_t>(cx + 1), cy, static_cast<int16_t>(x + 1), y, color);
        canvas.drawLine(cx, static_cast<int16_t>(cy + 1), x, static_cast<int16_t>(y + 1), color);
    }
}

void drawThickLine(RemoteCanvas& canvas,
                   int16_t x0,
                   int16_t y0,
                   int16_t x1,
                   int16_t y1,
                   int16_t halfWidth,
                   uint16_t color) {
    if(halfWidth <= 0) {
        canvas.drawLine(x0, y0, x1, y1, color);
        return;
    }

    const float dx = static_cast<float>(x1 - x0);
    const float dy = static_cast<float>(y1 - y0);
    const float length = sqrtf(dx * dx + dy * dy);
    if(length < 0.5f) {
        canvas.fillCircle(x0, y0, halfWidth, color);
        return;
    }

    const float nx = -dy / length;
    const float ny = dx / length;
    for(int16_t offset = static_cast<int16_t>(-halfWidth); offset <= halfWidth; ++offset) {
        const int16_t ox = static_cast<int16_t>(lroundf(nx * static_cast<float>(offset)));
        const int16_t oy = static_cast<int16_t>(lroundf(ny * static_cast<float>(offset)));
        canvas.drawLine(static_cast<int16_t>(x0 + ox),
                        static_cast<int16_t>(y0 + oy),
                        static_cast<int16_t>(x1 + ox),
                        static_cast<int16_t>(y1 + oy),
                        color);
    }
}

void drawRadialMarker(RemoteCanvas& canvas,
                      int16_t cx,
                      int16_t cy,
                      int16_t innerRadius,
                      int16_t outerRadius,
                      float angle,
                      int16_t halfWidth,
                      uint16_t color) {
    const int16_t x0 = static_cast<int16_t>(cx + lroundf(cosf(angle) * innerRadius));
    const int16_t y0 = static_cast<int16_t>(cy + lroundf(sinf(angle) * innerRadius));
    const int16_t x1 = static_cast<int16_t>(cx + lroundf(cosf(angle) * outerRadius));
    const int16_t y1 = static_cast<int16_t>(cy + lroundf(sinf(angle) * outerRadius));
    drawThickLine(canvas, x0, y0, x1, y1, halfWidth, color);
}

void drawWatchHand(RemoteCanvas& canvas,
                   int16_t cx,
                   int16_t cy,
                   int16_t radius,
                   float angle,
                   int16_t halfWidth,
                   uint16_t bodyColor,
                   uint16_t lumeColor,
                   int16_t tailRadius) {
    const int16_t tipX = static_cast<int16_t>(cx + lroundf(cosf(angle) * radius));
    const int16_t tipY = static_cast<int16_t>(cy + lroundf(sinf(angle) * radius));
    const int16_t tailX = static_cast<int16_t>(cx - lroundf(cosf(angle) * tailRadius));
    const int16_t tailY = static_cast<int16_t>(cy - lroundf(sinf(angle) * tailRadius));
    const int16_t lumeX = static_cast<int16_t>(cx + lroundf(cosf(angle) * (radius - 12)));
    const int16_t lumeY = static_cast<int16_t>(cy + lroundf(sinf(angle) * (radius - 12)));

    drawThickLine(canvas,
                  static_cast<int16_t>(tailX + 2),
                  static_cast<int16_t>(tailY + 2),
                  static_cast<int16_t>(tipX + 2),
                  static_cast<int16_t>(tipY + 2),
                  static_cast<int16_t>(halfWidth + 1),
                  blendColor565(kColorBlack, bodyColor, 0.18f));
    drawThickLine(canvas, tailX, tailY, tipX, tipY, halfWidth, bodyColor);
    drawThickLine(canvas, cx, cy, lumeX, lumeY, max<int16_t>(1, static_cast<int16_t>(halfWidth - 3)), lumeColor);
}

void drawDockClockInfoSection(RemoteCanvas& canvas,
                              int16_t centerX,
                              int16_t centerY,
                              int16_t width,
                              int16_t height,
                              const String& value,
                              RemoteTextStyle style,
                              uint16_t accent,
                              uint16_t faceBase,
                              uint16_t bg) {
    const String fitted = fitTextToWidthNoTilde(value,
                                                static_cast<int16_t>(width - 10),
                                                style,
                                                0);
    const int16_t textW = estimatedTextWidth(fitted, style);
    const int16_t tightW = clampI16(static_cast<int16_t>(textW + 18),
                                    static_cast<int16_t>(height + 16),
                                    width);
    const int16_t tightH = max<int16_t>(static_cast<int16_t>(height - 4), 28);
    const int16_t x = static_cast<int16_t>(centerX - tightW / 2);
    const int16_t y = static_cast<int16_t>(centerY - tightH / 2);
    const int16_t radius = static_cast<int16_t>(tightH / 2);
    const uint16_t face = blendColor565(faceBase, accent, 0.045f);
    const uint16_t border = blendColor565(face, accent, 0.16f);
    const uint16_t shadow = blendColor565(bg, kColorBlack, 0.10f);
    const uint16_t valueColor = blendColor565(accent, kColorText, 0.06f);

    canvas.fillRoundRect(x,
                         static_cast<int16_t>(y + 1),
                         tightW,
                         tightH,
                         radius,
                         shadow);
    canvas.fillRoundRect(x, y, tightW, tightH, radius, face);
    canvas.drawRoundRect(x, y, tightW, tightH, radius, border);
    canvas.drawCenteredText(fitted,
                            centerX,
                            static_cast<int16_t>(centerY + 1),
                            style,
                            valueColor,
                            face);
}

void drawDockWeatherIcon(RemoteCanvas& canvas,
                         uint16_t code,
                         int16_t cx,
                         int16_t cy,
                         int16_t size,
                         uint16_t accent,
                         uint16_t face) {
    const int16_t half = static_cast<int16_t>(size / 2);
    const uint16_t soft = blendColor565(face, accent, 0.22f);
    const uint16_t mark = blendColor565(accent, kColorText, 0.10f);

    auto drawCloud = [&]() {
        canvas.fillCircle(static_cast<int16_t>(cx - size / 5), cy, static_cast<int16_t>(size / 4), soft);
        canvas.fillCircle(cx, static_cast<int16_t>(cy - size / 6), static_cast<int16_t>(size / 3), soft);
        canvas.fillCircle(static_cast<int16_t>(cx + size / 4),
                          static_cast<int16_t>(cy + 1),
                          static_cast<int16_t>(size / 5),
                          soft);
        canvas.fillRoundRect(static_cast<int16_t>(cx - half),
                             static_cast<int16_t>(cy - 1),
                             size,
                             static_cast<int16_t>(size / 3),
                             static_cast<int16_t>(size / 6),
                             soft);
        canvas.drawLine(static_cast<int16_t>(cx - half),
                        static_cast<int16_t>(cy + size / 4),
                        static_cast<int16_t>(cx + half),
                        static_cast<int16_t>(cy + size / 4),
                        blendColor565(mark, face, 0.28f));
    };

    if(code == 0) {
        const int16_t radius = max<int16_t>(3, static_cast<int16_t>(size / 4));
        canvas.fillCircle(cx, cy, radius, soft);
        canvas.drawCircle(cx, cy, radius, mark);
        for(uint8_t i = 0; i < 8; ++i) {
            const float angle = static_cast<float>(i) * static_cast<float>(PI / 4.0f);
            const int16_t x0 = static_cast<int16_t>(cx + lroundf(cosf(angle) * (radius + 3)));
            const int16_t y0 = static_cast<int16_t>(cy + lroundf(sinf(angle) * (radius + 3)));
            const int16_t x1 = static_cast<int16_t>(cx + lroundf(cosf(angle) * half));
            const int16_t y1 = static_cast<int16_t>(cy + lroundf(sinf(angle) * half));
            canvas.drawLine(x0, y0, x1, y1, blendColor565(mark, face, 0.18f));
        }
        return;
    }

    if(code == 45 || code == 48) {
        for(uint8_t i = 0; i < 3; ++i) {
            const int16_t y = static_cast<int16_t>(cy - size / 4 + i * size / 4);
            canvas.drawLine(static_cast<int16_t>(cx - half), y, static_cast<int16_t>(cx + half), y, mark);
        }
        return;
    }

    drawCloud();

    if(code >= 95) {
        drawBoltIcon(canvas,
                     static_cast<int16_t>(cx + size / 8),
                     static_cast<int16_t>(cy + size / 3),
                     static_cast<int16_t>(size / 2),
                     mark);
        return;
    }

    if((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        for(int8_t i = -1; i <= 1; ++i) {
            const int16_t x = static_cast<int16_t>(cx + i * size / 4);
            canvas.drawLine(x,
                            static_cast<int16_t>(cy + size / 3),
                            static_cast<int16_t>(x - size / 8),
                            static_cast<int16_t>(cy + half),
                            mark);
        }
        return;
    }

    if((code >= 71 && code <= 77) || code == 85 || code == 86) {
        const int16_t sy = static_cast<int16_t>(cy + size / 3);
        canvas.drawLine(static_cast<int16_t>(cx - size / 5), sy, static_cast<int16_t>(cx + size / 5), sy, mark);
        canvas.drawLine(cx, static_cast<int16_t>(sy - size / 5), cx, static_cast<int16_t>(sy + size / 5), mark);
        canvas.drawLine(static_cast<int16_t>(cx - size / 6),
                        static_cast<int16_t>(sy - size / 6),
                        static_cast<int16_t>(cx + size / 6),
                        static_cast<int16_t>(sy + size / 6),
                        mark);
        canvas.drawLine(static_cast<int16_t>(cx - size / 6),
                        static_cast<int16_t>(sy + size / 6),
                        static_cast<int16_t>(cx + size / 6),
                        static_cast<int16_t>(sy - size / 6),
                        mark);
    }
}

void drawDockClockWeatherSection(RemoteCanvas& canvas,
                                 int16_t centerX,
                                 int16_t centerY,
                                 int16_t width,
                                 int16_t height,
                                 const String& value,
                                 RemoteTextStyle style,
                                 uint16_t accent,
                                 uint16_t faceBase,
                                 uint16_t bg) {
    const int16_t iconSize = clampI16(static_cast<int16_t>(height - 12), 16, 22);
    const int16_t gap = 7;
    const int16_t padding = 10;
    const String fitted = fitTextToWidthNoTilde(value,
                                                static_cast<int16_t>(width - iconSize - gap - padding * 2),
                                                style,
                                                0);
    const int16_t textW = estimatedTextWidth(fitted, style);
    const int16_t tightW = clampI16(static_cast<int16_t>(textW + iconSize + gap + padding * 2),
                                    static_cast<int16_t>(height + 42),
                                    width);
    const int16_t tightH = max<int16_t>(static_cast<int16_t>(height - 4), 28);
    const int16_t x = static_cast<int16_t>(centerX - tightW / 2);
    const int16_t y = static_cast<int16_t>(centerY - tightH / 2);
    const int16_t radius = static_cast<int16_t>(tightH / 2);
    const uint16_t face = blendColor565(faceBase, accent, 0.045f);
    const uint16_t border = blendColor565(face, accent, 0.16f);
    const uint16_t shadow = blendColor565(bg, kColorBlack, 0.10f);
    const uint16_t valueColor = blendColor565(accent, kColorText, 0.06f);

    canvas.fillRoundRect(x,
                         static_cast<int16_t>(y + 1),
                         tightW,
                         tightH,
                         radius,
                         shadow);
    canvas.fillRoundRect(x, y, tightW, tightH, radius, face);
    canvas.drawRoundRect(x, y, tightW, tightH, radius, border);

    const int16_t iconCx = static_cast<int16_t>(x + padding + iconSize / 2);
    drawDockWeatherIcon(canvas, app.dockWeather.weatherCode, iconCx, centerY, iconSize, accent, face);

    const int16_t textX = static_cast<int16_t>(x + padding + iconSize + gap);
    const int16_t textWAvailable = static_cast<int16_t>(tightW - padding - iconSize - gap - padding);
    canvas.drawCenteredText(fitted,
                            static_cast<int16_t>(textX + textWAvailable / 2),
                            static_cast<int16_t>(centerY + 1),
                            style,
                            valueColor,
                            face);
}

void drawDockClock(RemoteCanvas& canvas, const Layout& layout) {
    const bool roundClock = useRoundNavigation();
    const uint16_t bg = roundClock ? blendColor565(kColorBlack, packColor565(0, 20, 44), 0.58f)
                                   : blendColor565(kColorBg, packColor565(0, 18, 28), 0.46f);
    const uint16_t accent = dockWeatherAccent();
    const uint16_t grid = roundClock ? blendColor565(packColor565(132, 180, 210), bg, 0.42f)
                                     : blendColor565(packColor565(74, 210, 164), bg, 0.34f);
    const uint16_t text = roundClock ? blendColor565(packColor565(226, 236, 226), accent, 0.08f)
                                     : blendColor565(kColorText, accent, 0.12f);
    const uint16_t muted = roundClock ? blendColor565(packColor565(130, 154, 168), accent, 0.10f)
                                      : blendColor565(kColorMuted, accent, 0.18f);
    canvas.fillScreen(bg);
    drawDockWeatherBackdrop(canvas, layout, bg, accent);

    struct tm timeInfo = {};
    const bool hasTime = localClockTime(&timeInfo);
    if(!hasTime) {
        timeInfo.tm_hour = 12;
        timeInfo.tm_min = 0;
        timeInfo.tm_sec = 0;
    }

    const int16_t cx = static_cast<int16_t>(layout.width / 2);
    const int16_t cy = static_cast<int16_t>(layout.height / 2 + (useRoundNavigation() ? 8 : 4));
    const int16_t shortSide = min<int16_t>(layout.width, layout.height);
    const int16_t radius = clampI16(static_cast<int16_t>(shortSide / 2 - (useRoundNavigation() ? 66 : 50)),
                                    92,
                                    static_cast<int16_t>(shortSide / 2 - 28));
    const uint16_t face = roundClock ? blendColor565(packColor565(2, 18, 36), kColorBlack, 0.20f)
                                     : blendColor565(bg, kColorBlack, 0.10f);
    if(roundClock) {
        const int16_t bezelOuter = min<int16_t>(static_cast<int16_t>(radius + 22),
                                                static_cast<int16_t>(shortSide / 2 - 10));
        const int16_t bezelMid = static_cast<int16_t>(radius + 14);
        const int16_t bezelInner = static_cast<int16_t>(radius + 5);
        const uint16_t bezelDark = blendColor565(kColorBlack, packColor565(4, 26, 54), 0.66f);
        const uint16_t bezelSteel = blendColor565(packColor565(72, 92, 108), packColor565(12, 42, 78), 0.44f);
        const uint16_t bezelEdge = blendColor565(packColor565(150, 178, 192), bg, 0.32f);
        const uint16_t lume = blendColor565(packColor565(218, 246, 216), accent, 0.08f);

        canvas.fillCircle(cx, cy, bezelOuter, blendColor565(kColorBlack, packColor565(0, 12, 24), 0.44f));
        canvas.drawCircle(cx, cy, bezelOuter, bezelEdge);
        canvas.fillCircle(cx, cy, bezelMid, bezelSteel);
        canvas.drawCircle(cx, cy, static_cast<int16_t>(bezelMid - 2), blendColor565(bezelEdge, kColorBlack, 0.18f));
        canvas.fillCircle(cx, cy, bezelInner, bezelDark);
        canvas.fillCircle(cx, cy, radius, face);
        canvas.drawCircle(cx, cy, radius, blendColor565(bezelEdge, accent, 0.08f));
        canvas.drawCircle(cx, cy, static_cast<int16_t>(radius - 8), blendColor565(grid, face, 0.22f));
        canvas.drawCircle(cx, cy, static_cast<int16_t>(radius - 38), blendColor565(grid, face, 0.42f));

        for(uint8_t i = 0; i < 60; ++i) {
            const float angle = static_cast<float>(-PI / 2.0f) +
                                static_cast<float>(i) * static_cast<float>(2.0f * PI / 60.0f);
            const bool major = (i % 5) == 0;
            const uint16_t tickColor = major ? blendColor565(bezelEdge, lume, 0.28f)
                                             : blendColor565(bezelEdge, bg, 0.26f);
            drawRadialMarker(canvas,
                             cx,
                             cy,
                             static_cast<int16_t>(radius + (major ? 7 : 10)),
                             static_cast<int16_t>(radius + (major ? 18 : 16)),
                             angle,
                             major ? 1 : 0,
                             tickColor);
        }

        for(uint8_t i = 0; i < 12; ++i) {
            const float angle = static_cast<float>(-PI / 2.0f) +
                                static_cast<float>(i) * static_cast<float>(2.0f * PI / 12.0f);
            const bool cardinal = (i % 3) == 0;
            const int16_t inner = static_cast<int16_t>(radius - (cardinal ? 42 : 34));
            const int16_t outer = static_cast<int16_t>(radius - 13);
            drawRadialMarker(canvas,
                             cx,
                             cy,
                             inner,
                             outer,
                             angle,
                             cardinal ? 4 : 3,
                             blendColor565(lume, kColorText, cardinal ? 0.16f : 0.06f));
            drawRadialMarker(canvas,
                             cx,
                             cy,
                             static_cast<int16_t>(inner + 4),
                             static_cast<int16_t>(outer - 4),
                             angle,
                             cardinal ? 2 : 1,
                             blendColor565(packColor565(82, 126, 146), lume, 0.36f));
        }
    } else {
        canvas.fillCircle(cx, cy, radius, face);
        canvas.drawCircle(cx, cy, radius, blendColor565(grid, accent, 0.18f));
        canvas.drawCircle(cx, cy, static_cast<int16_t>(radius - 8), blendColor565(grid, bg, 0.16f));

        for(uint8_t i = 0; i < 60; ++i) {
            const float angle = static_cast<float>(-PI / 2.0f) + static_cast<float>(i) * static_cast<float>(2.0f * PI / 60.0f);
            const bool major = (i % 5) == 0;
            const int16_t outer = static_cast<int16_t>(radius - (major ? 3 : 8));
            const int16_t inner = static_cast<int16_t>(radius - (major ? 18 : 13));
            const int16_t x0 = static_cast<int16_t>(cx + lroundf(cosf(angle) * inner));
            const int16_t y0 = static_cast<int16_t>(cy + lroundf(sinf(angle) * inner));
            const int16_t x1 = static_cast<int16_t>(cx + lroundf(cosf(angle) * outer));
            const int16_t y1 = static_cast<int16_t>(cy + lroundf(sinf(angle) * outer));
            canvas.drawLine(x0, y0, x1, y1, major ? blendColor565(text, accent, 0.18f) : blendColor565(grid, bg, 0.30f));
        }
    }

    const float secondAngle = static_cast<float>(-PI / 2.0f) +
                              static_cast<float>(timeInfo.tm_sec) * static_cast<float>(2.0f * PI / 60.0f);
    const float minuteAngle = static_cast<float>(-PI / 2.0f) +
                              (static_cast<float>(timeInfo.tm_min) + static_cast<float>(timeInfo.tm_sec) / 60.0f) *
                                  static_cast<float>(2.0f * PI / 60.0f);
    const float hourAngle = static_cast<float>(-PI / 2.0f) +
                            (static_cast<float>(timeInfo.tm_hour % 12) + static_cast<float>(timeInfo.tm_min) / 60.0f) *
                                static_cast<float>(2.0f * PI / 12.0f);

    if(!roundClock) {
        drawClockHand(canvas, cx, cy, static_cast<int16_t>(radius * 0.48f), hourAngle, text, true);
        drawClockHand(canvas, cx, cy, static_cast<int16_t>(radius * 0.72f), minuteAngle, blendColor565(text, accent, 0.12f), true);
        drawClockHand(canvas, cx, cy, static_cast<int16_t>(radius * 0.82f), secondAngle, accent, false);
        canvas.fillCircle(cx, cy, 7, blendColor565(accent, kColorText, 0.20f));
        canvas.drawCircle(cx, cy, 12, blendColor565(grid, accent, 0.16f));
    }

    char dateText[24] = {};
    char shortDateText[8] = {};
    if(hasTime) {
        strftime(dateText, sizeof(dateText), "%a %b %d", &timeInfo);
        strftime(shortDateText, sizeof(shortDateText), "%m/%d", &timeInfo);
    } else {
        dateText[0] = '\0';
        shortDateText[0] = '\0';
    }

    const String location = dockLocationLine();
    if(!useRoundNavigation() && location.length() > 0) {
        canvas.drawCenteredText(location,
                                cx,
                                static_cast<int16_t>(layout.pad + 14),
                                RemoteTextStyle::Small,
                                muted,
                                bg);
    }
    const String weather = roundClock ? dockWeatherMetricLine() : dockWeatherLine();
    const String gapText = dockGapLine();
    if(roundClock) {
        const uint16_t gapAccent = blendColor565(packColor565(255, 150, 48), accent, 0.16f);
        const uint16_t dateAccent = blendColor565(packColor565(96, 236, 174), accent, 0.14f);
        const uint16_t weatherAccent = blendColor565(packColor565(88, 190, 255), accent, 0.20f);
        const uint16_t placeAccent = blendColor565(packColor565(178, 160, 255), accent, 0.12f);
        const uint16_t sectionFace = blendColor565(face, bg, 0.18f);
        const int16_t sideOffsetX = static_cast<int16_t>(radius * 0.54f);
        const int16_t verticalOffsetY = static_cast<int16_t>(radius * 0.58f);
        const int16_t topY = static_cast<int16_t>(cy - verticalOffsetY);
        const int16_t bottomY = static_cast<int16_t>(cy + verticalOffsetY);
        const int16_t sideY = cy;
        const int16_t weatherW = clampI16(static_cast<int16_t>(radius * 2 - 54), 198, 258);
        const int16_t timeW = clampI16(static_cast<int16_t>(radius + 30), 142, 184);
        const int16_t infoW = clampI16(static_cast<int16_t>(radius - 24), 112, 146);

        if(weather.length() > 0) {
            drawDockClockWeatherSection(canvas,
                                        cx,
                                        topY,
                                        weatherW,
                                        34,
                                        weather,
                                        tinyTextStyle(),
                                        weatherAccent,
                                        sectionFace,
                                        bg);
        }
        if(location.length() > 0) {
            drawDockClockInfoSection(canvas,
                                     static_cast<int16_t>(cx - sideOffsetX),
                                     sideY,
                                     infoW,
                                     34,
                                     location,
                                     tinyTextStyle(),
                                     placeAccent,
                                     sectionFace,
                                     bg);
        }
        if(hasTime) {
            drawDockClockInfoSection(canvas,
                                     static_cast<int16_t>(cx + sideOffsetX),
                                     sideY,
                                     infoW,
                                     34,
                                     shortDateText,
                                     tinyTextStyle(),
                                     dateAccent,
                                     sectionFace,
                                     bg);
        }
        if(kShowBurrGapReadout) {
            drawDockClockInfoSection(canvas,
                                     cx,
                                     bottomY,
                                     timeW,
                                     38,
                                     gapText,
                                     RemoteTextStyle::Medium,
                                     gapAccent,
                                     sectionFace,
                                     bg);
        }
        const uint16_t handBody = blendColor565(packColor565(182, 198, 205), kColorText, 0.20f);
        const uint16_t handLume = blendColor565(packColor565(196, 246, 218), accent, 0.08f);
        const uint16_t minuteBody = blendColor565(handBody, packColor565(92, 116, 132), 0.20f);
        const uint16_t secondRed = packColor565(255, 48, 42);
        drawWatchHand(canvas,
                      cx,
                      cy,
                      static_cast<int16_t>(radius * 0.48f),
                      hourAngle,
                      5,
                      handBody,
                      handLume,
                      18);
        drawWatchHand(canvas,
                      cx,
                      cy,
                      static_cast<int16_t>(radius * 0.72f),
                      minuteAngle,
                      4,
                      minuteBody,
                      handLume,
                      24);
        drawThickLine(canvas,
                      static_cast<int16_t>(cx - lroundf(cosf(secondAngle) * radius * 0.18f)),
                      static_cast<int16_t>(cy - lroundf(sinf(secondAngle) * radius * 0.18f)),
                      static_cast<int16_t>(cx + lroundf(cosf(secondAngle) * radius * 0.82f)),
                      static_cast<int16_t>(cy + lroundf(sinf(secondAngle) * radius * 0.82f)),
                      1,
                      secondRed);
        canvas.fillCircle(cx, cy, 8, blendColor565(handBody, kColorText, 0.16f));
        canvas.fillCircle(cx, cy, 4, secondRed);
        canvas.drawCircle(cx, cy, 13, blendColor565(secondRed, kColorText, 0.12f));
    } else {
        if(kShowBurrGapReadout) {
            canvas.drawCenteredText(gapText,
                                    cx,
                                    static_cast<int16_t>(cy + radius + 24),
                                    RemoteTextStyle::Medium,
                                    blendColor565(packColor565(255, 150, 48), text, 0.18f),
                                    bg);
        }
        if(hasTime) {
            canvas.drawCenteredText(dateText,
                                    cx,
                                    static_cast<int16_t>(cy + radius + 44),
                                    tinyTextStyle(),
                                    muted,
                                    bg);
        }
        if(weather.length() > 0) {
            canvas.drawCenteredText(fitTextToWidth(weather,
                                                   static_cast<int16_t>(layout.width - layout.pad * 5),
                                                   tinyTextStyle(),
                                                   0),
                                    cx,
                                    static_cast<int16_t>(layout.height - layout.pad - 12),
                                    tinyTextStyle(),
                                    blendColor565(text, accent, 0.22f),
                                    bg);
        }
    }
    const int16_t connectionW = roundClock ? static_cast<int16_t>(shortSide / 3)
                                           : static_cast<int16_t>(layout.width - layout.pad * 4);
    const String connectionLabel = isLinkOnline() ? "CTRL ONLINE" : "CTRL OFFLINE";
    canvas.drawCenteredText(fitTextToWidth(connectionLabel, connectionW, RemoteTextStyle::Small, 0),
                            cx,
                            roundClock ? 22 : static_cast<int16_t>(layout.pad + 30),
                            RemoteTextStyle::Small,
                            linkStatusColor(),
                            bg);
    canvas.drawCenteredText(fitTextToWidth(networkCurveStatusText(),
                                           roundClock ? static_cast<int16_t>(shortSide / 2) : connectionW,
                                           RemoteTextStyle::Tiny, 0),
                            cx,
                            roundClock ? 40 : static_cast<int16_t>(layout.pad + 46),
                            RemoteTextStyle::Tiny,
                            muted,
                            bg);
}

void drawPressureProfiler(RemoteCanvas& canvas, const Layout& layout) {
    const uint16_t bg = blendColor565(kColorBg, packColor565(0, 22, 18), 0.52f);
    const uint16_t accent = blendColor565(kColorGood, packColor565(88, 255, 170), 0.30f);
    const uint16_t grid = blendColor565(packColor565(16, 176, 108), bg, 0.26f);
    const uint16_t labelColor = blendColor565(accent, bg, 0.16f);
    const uint16_t dimLabel = blendColor565(accent, bg, 0.48f);
    canvas.fillScreen(bg);

    const String state = !g_pressureProfiler.adcReady
                             ? "ADC?"
                             : (g_pressureProfiler.running ? pressureElapsedLabel(millis() - g_pressureProfiler.startedAt)
                                                           : "ARM");

    uint32_t visibleEndMs = remote_config::kPressureProfilerVisibleWindowMs;
    if(g_pressureProfiler.running) {
        visibleEndMs = max<uint32_t>(millis() - g_pressureProfiler.startedAt,
                                     remote_config::kPressureProfilerVisibleWindowMs);
    }
    uint32_t visibleStartMs = 0;
    if(visibleEndMs > remote_config::kPressureProfilerVisibleWindowMs) {
        visibleStartMs = visibleEndMs - remote_config::kPressureProfilerVisibleWindowMs;
    }

    Rect graph = {};
    if(useRoundNavigation()) {
        const int16_t shortSide = min<int16_t>(layout.width, layout.height);
        const int16_t topReserved = clampI16(static_cast<int16_t>(layout.pad + 52), 62, 76);
        const int16_t bottomReserved = clampI16(static_cast<int16_t>(layout.pad + 76), 86, 100);
        int16_t graphW = clampI16(static_cast<int16_t>((shortSide * 74) / 100),
                                  static_cast<int16_t>((shortSide * 62) / 100),
                                  static_cast<int16_t>(layout.width - 112));
        int16_t graphH = clampI16(static_cast<int16_t>((shortSide * 46) / 100),
                                  static_cast<int16_t>((shortSide * 36) / 100),
                                  static_cast<int16_t>(layout.height - topReserved - bottomReserved));
        const float safeRadius = static_cast<float>(shortSide / 2 - 18);
        const float cornerRadius = sqrtf(static_cast<float>(graphW * graphW + graphH * graphH)) * 0.5f;
        if(cornerRadius > safeRadius && cornerRadius > 0.0f) {
            const float scale = safeRadius / cornerRadius;
            graphW = static_cast<int16_t>(graphW * scale);
            graphH = static_cast<int16_t>(graphH * scale);
        }
        graph = {
            static_cast<int16_t>((layout.width - graphW) / 2),
            static_cast<int16_t>((layout.height - graphH) / 2 + 4),
            graphW,
            graphH
        };
    } else {
        const int16_t leftMargin = static_cast<int16_t>(layout.pad + 28);
        const int16_t topMargin = static_cast<int16_t>(layout.pad + layout.topPillH + 8);
        const int16_t rightMargin = static_cast<int16_t>(layout.pad + 12);
        const int16_t bottomMargin = static_cast<int16_t>(layout.pad + 34);
        graph = {
            leftMargin,
            topMargin,
            static_cast<int16_t>(layout.width - leftMargin - rightMargin),
            static_cast<int16_t>(layout.height - topMargin - bottomMargin)
        };
    }
    const uint16_t traceColor = g_pressureProfiler.running ? packColor565(255, 36, 28) : accent;
    drawPressureProfilerGraph(canvas, graph, visibleStartMs, visibleEndMs, traceColor, accent, bg, grid);

    const int16_t centerX = static_cast<int16_t>(layout.width / 2);
    const uint16_t current = g_pressureProfiler.currentCentiBar;
    const String value = pressureBarLabel(current);
    const RemoteTextStyle valueStyle = isLargeCanvas() ? RemoteTextStyle::Large : RemoteTextStyle::Medium;
    const String peak = pressureBarLabel(g_pressureProfiler.peakCentiBar);
    const String sense = pressureBarLabel(remote_config::kPressureProfilerSenseCentiBar);
    const String ble = remote_pressure_ble_connected()
                           ? (remote_pressure_ble_streaming() ? "BC*" : "BC")
                           : "BC";
    const uint16_t bleColor = remote_pressure_ble_connected()
                                  ? (remote_pressure_ble_streaming()
                                         ? packColor565(120, 255, 166)
                                         : packColor565(78, 238, 126))
                                  : blendColor565(bg, kColorMuted, 0.26f);
    if(useRoundNavigation()) {
        const String topReadout = value + " BAR";
        canvas.drawCenteredText(fitTextToWidth(topReadout,
                                               static_cast<int16_t>(layout.width / 2),
                                               RemoteTextStyle::Small,
                                               0),
                                centerX,
                                static_cast<int16_t>(layout.pad + 20),
                                RemoteTextStyle::Small,
                                blendColor565(accent, bg, 0.24f),
                                bg);
        canvas.drawCenteredText(state,
                                centerX,
                                static_cast<int16_t>(layout.pad + 42),
                                tinyTextStyle(),
                                g_pressureProfiler.running ? labelColor : dimLabel,
                                bg);

        const int16_t bottomY = min<int16_t>(
            static_cast<int16_t>(layout.height - layout.pad - 44),
            static_cast<int16_t>(graph.y + graph.h + 56));
        const String middle = app.toastUntil > millis() ? upperText(app.toast) : String("S ") + sense;
        canvas.drawCenteredText(String("PK ") + peak,
                                static_cast<int16_t>(centerX - graph.w / 4),
                                bottomY,
                                tinyTextStyle(),
                                dimLabel,
                                bg);
        canvas.drawCenteredText(middle,
                                centerX,
                                bottomY,
                                tinyTextStyle(),
                                dimLabel,
                                bg);
        canvas.drawCenteredText(ble,
                                static_cast<int16_t>(centerX + graph.w / 4),
                                bottomY,
                                tinyTextStyle(),
                                bleColor,
                                bg);
    } else {
        canvas.drawTextTopLeft(fitTextToWidth(value,
                                              static_cast<int16_t>(graph.w / 2),
                                              valueStyle,
                                              0),
                               static_cast<int16_t>(layout.pad + 2),
                               static_cast<int16_t>(layout.pad + 2),
                               valueStyle,
                               labelColor,
                               bg);
        canvas.drawTextTopLeft("BAR",
                               static_cast<int16_t>(layout.pad + 4),
                               static_cast<int16_t>(layout.pad + (isLargeCanvas() ? 34 : 24)),
                               tinyTextStyle(),
                               dimLabel,
                               bg);
        canvas.drawCenteredText(state,
                                centerX,
                                static_cast<int16_t>(layout.pad + 12),
                                RemoteTextStyle::Small,
                                g_pressureProfiler.running ? labelColor : dimLabel,
                                bg);

        const String footer = g_pressureProfiler.running
                                  ? String("PK ") + peak + "  " + ble
                                  : (app.toastUntil > millis() ? upperText(app.toast) : String("SENSE ") + sense);
        canvas.drawCenteredText(fitTextToWidth(footer,
                                               static_cast<int16_t>(layout.width - layout.pad * 4),
                                               tinyTextStyle(),
                                               0),
                                centerX,
                                static_cast<int16_t>(layout.height - layout.pad - 12),
                                tinyTextStyle(),
                                dimLabel,
                                bg);
    }
    drawBatteryIndicator(canvas, layout, bg);
}

String aboutOtaLine() {
    return esp_ota_get_next_update_partition(nullptr) != nullptr ? "APP SLOT" : "FULL FLASH";
}

String aboutBuildTimeLine() {
    const String date = SG98_BUILD_DATE;
    const String time = SG98_BUILD_TIME;
    if(date.length() >= 10 && time.length() >= 5) {
        return date.substring(5, 7) + "/" + date.substring(8, 10) + " " + time.substring(0, 5) + "Z";
    }
    String out = date;
    out += " ";
    out += time;
    return out;
}

String aboutRoleLine() {
    String out = sg98_version::role();
    out.toUpperCase();
    return out;
}

bool remoteOtaUpdateAvailable() {
    return app.otaUpdate.checked && app.otaUpdate.updateAvailable;
}

void drawAboutRow(RemoteCanvas& canvas,
                  const Rect& row,
                  const String& label,
                  const String& value,
                  uint16_t face,
                  uint16_t labelColor,
                  uint16_t valueColor) {
    const RemoteTextStyle labelStyle = RemoteTextStyle::Tiny;
    const int16_t valueWidth = static_cast<int16_t>(row.w - label.length() * 6 - 12);
    const RemoteTextStyle valueStyle = textFitsWidth(value, valueWidth, RemoteTextStyle::Small, 0)
                                          ? RemoteTextStyle::Small : RemoteTextStyle::Tiny;
    canvas.drawTextTopLeft(upperText(label),
                           row.x,
                           row.y,
                           labelStyle,
                           labelColor,
                           face);
    canvas.drawTextTopRight(fitTextToWidthNoTilde(value, valueWidth, valueStyle, 0),
                            static_cast<int16_t>(row.x + row.w),
                            row.y,
                            valueStyle,
                            valueColor,
                            face);
}

void drawAboutUpdateBanner(RemoteCanvas& canvas,
                           const Rect& rect,
                           bool remoteUpdateReady,
                           bool armed,
                           uint16_t fill,
                           uint16_t textColor) {
    (void)remoteUpdateReady;
    const String top = armed ? "PRESS AGAIN" : "SYSTEM UPDATE";
    const String bottom = armed ? "UPDATE BOTH" : "CONTROLLER + REMOTE";

    if(useRoundNavigation()) {
        const uint16_t face = blendColor565(kColorPanel, fill, 0.34f);
        const uint16_t border = blendColor565(face, kColorBg, 0.05f);
        drawNestCard(canvas, rect, face, border, true);
        const RemoteTextStyle topStyle =
            textFitsWidth(top, rect.w, RemoteTextStyle::Small, 16)
                ? RemoteTextStyle::Small
                : RemoteTextStyle::Tiny;
        const RemoteTextStyle bottomStyle =
            textFitsWidth(bottom, rect.w, RemoteTextStyle::Medium, 16)
                ? RemoteTextStyle::Medium
                : RemoteTextStyle::Small;
        const int16_t centerX = static_cast<int16_t>(rect.x + rect.w / 2);
        const uint16_t topTextColor = armed
                                          ? textColor
                                          : blendColor565(textColor, face, 0.18f);
        drawSoftCenteredText(canvas,
                             fitTextToWidth(top, rect.w, topStyle, 16),
                             centerX,
                             static_cast<int16_t>(rect.y + rect.h / 2 - 11),
                             topStyle,
                             topTextColor,
                             face,
                             fill,
                             !armed);
        drawSoftCenteredText(canvas,
                             fitTextToWidth(bottom, rect.w, bottomStyle, 16),
                             centerX,
                             static_cast<int16_t>(rect.y + rect.h / 2 + 11),
                             bottomStyle,
                             textColor,
                             face,
                             fill,
                             false);
        return;
    }

    const int16_t radius = rect.h / 2;
    canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, radius, fill);
    canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, radius, kColorPanelHi);
    const RemoteTextStyle topStyle = tinyTextStyle();
    const RemoteTextStyle bottomStyle =
        textFitsWidth(bottom, rect.w, RemoteTextStyle::Small, 12)
            ? RemoteTextStyle::Small
            : tinyTextStyle();
    const int16_t centerX = static_cast<int16_t>(rect.x + rect.w / 2);
    canvas.drawCenteredText(fitTextToWidth(top, rect.w, topStyle, 12),
                            centerX,
                            static_cast<int16_t>(rect.y + rect.h / 2 - 7),
                            topStyle,
                            textColor,
                            fill);
    canvas.drawCenteredText(fitTextToWidth(bottom, rect.w, bottomStyle, 12),
                            centerX,
                            static_cast<int16_t>(rect.y + rect.h / 2 + 7),
                            bottomStyle,
                            textColor,
                            fill);
}

void drawAboutPage(RemoteCanvas& canvas, const Layout& layout) {
    canvas.fillScreen(kColorBg);
    drawPill(canvas, layout.modeBadge, "ABOUT", kColorPanel, kColorText);
    drawBatteryIndicator(canvas, layout, kColorBg);

    const Rect content = useRoundNavigation()
                             ? Rect(static_cast<int16_t>(layout.width / 8),
                                    layout.listTop,
                                    static_cast<int16_t>(layout.width * 3 / 4),
                                    static_cast<int16_t>(layout.listBottom - layout.listTop))
                             : Rect(static_cast<int16_t>(layout.pad * 2),
                                    layout.listTop,
                                    static_cast<int16_t>(layout.width - layout.pad * 4),
                                    static_cast<int16_t>(layout.listBottom - layout.listTop - layout.gap));
    const int16_t panelPad = useRoundNavigation() ? 18 : 14;
    const int16_t maxPanelW = useRoundNavigation() ? 334 : content.w;
    const int16_t panelW = min<int16_t>(maxPanelW, static_cast<int16_t>(content.w - (useRoundNavigation() ? 12 : 0)));
    const int16_t panelX = static_cast<int16_t>(content.x + (content.w - panelW) / 2);
    const int16_t panelTop = useRoundNavigation()
                                 ? static_cast<int16_t>(layout.modeBadge.y + layout.modeBadge.h + layout.gap + 8)
                                 : content.y;
    const int16_t panelBottom = useRoundNavigation()
                                    ? static_cast<int16_t>(layout.footer.y + layout.footer.h - 2)
                                    : static_cast<int16_t>(content.y + content.h);
    const Rect panel = {
        panelX,
        panelTop,
        panelW,
        static_cast<int16_t>(panelBottom - panelTop)
    };
    const uint16_t accent = blendColor565(kColorAccent, kColorText, 0.08f);
    const uint16_t face = blendColor565(kColorPanel, kColorBg, 0.24f);
    drawNestCard(canvas, panel, face, blendColor565(accent, face, 0.18f), true);

    const int16_t innerX = static_cast<int16_t>(panel.x + panelPad);
    const int16_t innerW = static_cast<int16_t>(panel.w - panelPad * 2);
    const int16_t versionY = static_cast<int16_t>(panel.y + panelPad + (useRoundNavigation() ? 6 : 10));
    const RemoteTextStyle versionStyle = textFitsWidth(sg98_version::shortVersion(), innerW, RemoteTextStyle::Medium, 2)
                                            ? RemoteTextStyle::Medium : RemoteTextStyle::Small;
    const RemoteTextStyle envStyle = textFitsWidth(sg98_version::buildEnv(), innerW, RemoteTextStyle::Small, 2)
                                        ? RemoteTextStyle::Small : RemoteTextStyle::Tiny;
    canvas.drawCenteredText(fitTextToWidth(sg98_version::shortVersion(), innerW, versionStyle, 2),
                            static_cast<int16_t>(panel.x + panel.w / 2),
                            versionY,
                            versionStyle,
                            accent,
                            face);
    canvas.drawCenteredText(aboutRoleLine(),
                            static_cast<int16_t>(panel.x + panel.w / 2),
                            static_cast<int16_t>(versionY + (useRoundNavigation() ? 26 : 24)),
                            tinyTextStyle(),
                            blendColor565(kColorMuted, face, 0.02f),
                            face);
    canvas.drawCenteredText(fitTextToWidthNoTilde(String(sg98_version::buildEnv()), innerW, envStyle, 2),
                            static_cast<int16_t>(panel.x + panel.w / 2),
                            static_cast<int16_t>(versionY + (useRoundNavigation() ? 48 : 42)),
                            envStyle,
                            kColorText,
                            face);

    const int16_t dividerY = static_cast<int16_t>(versionY + (useRoundNavigation() ? 66 : 58));
    canvas.drawLine(innerX,
                    dividerY,
                    static_cast<int16_t>(innerX + innerW),
                    dividerY,
                    blendColor565(face, accent, 0.22f));

    const sg98_system_ota::Stage updateStage = g_systemOta.flow.stage;
    const bool updateSucceeded = updateStage == sg98_system_ota::Stage::Complete;
    const bool updateFailed = updateStage == sg98_system_ota::Stage::Failed;
    const char* labels[] = {"GIT", "BUILT", "CTRL",
                            updateSucceeded ? "SUCCESS" : (updateFailed ? "FAILED" : "UPDATE")};
    const String values[] = {
        sg98_version::gitLabel(),
        aboutBuildTimeLine(),
        g_systemOta.controllerVersion.isEmpty() ? g_systemOta.controllerUi
                                              : g_systemOta.controllerVersion + (g_systemOta.controllerLocalUi ? " Full" : " Internal"),
        g_systemOta.detail.isEmpty() ? String("Controller + remote") : g_systemOta.detail
    };
    const uint8_t rowCount = static_cast<uint8_t>(sizeof(labels) / sizeof(labels[0]));
    const int16_t rowH = useRoundNavigation() ? 24 : 22;
    int16_t rowY = static_cast<int16_t>(dividerY + (useRoundNavigation() ? 10 : 9));
    const uint16_t labelColor = blendColor565(kColorMuted, face, 0.04f);
    for(uint8_t i = 0; i < rowCount; ++i) {
        const Rect row = {innerX, rowY, innerW, rowH};
        const bool resultRow = i == 3 && (updateSucceeded || updateFailed);
        const uint16_t resultColor = updateSucceeded ? kColorGood : kColorDanger;
        const uint16_t rowFace = resultRow ? blendColor565(face, resultColor, 0.18f) : face;
        if(resultRow) canvas.fillRect(row.x, row.y - 2, row.w, row.h, rowFace);
        drawAboutRow(canvas,
                     row,
                     labels[i],
                     values[i],
                     rowFace,
                     resultRow ? resultColor : labelColor,
                     resultRow ? resultColor
                               : (i == 2 || (i == 3 && app.otaUpdate.updateAvailable)
                                      ? blendColor565(kColorGood, kColorText, 0.10f)
                                      : kColorText));
        rowY = static_cast<int16_t>(rowY + rowH);
    }

    const bool controllerUpdateReady = controllerOtaReady();
    const bool remoteUpdateReady = remoteOtaUpdateAvailable() && !controllerUpdateReady;
    {
        const int16_t buttonH = useRoundNavigation() ? 48 : 34;
        const int16_t buttonY = max<int16_t>(
            static_cast<int16_t>(rowY + (useRoundNavigation() ? 10 : 6)),
            static_cast<int16_t>(panel.y + panel.h - panelPad - buttonH));
        const Rect button = {innerX, buttonY, innerW, buttonH};
        const uint8_t dangerSlot = 2;
        const bool armed = dangerIsArmed(Screen::About, dangerSlot);
        const uint16_t buttonFace = armed ? kColorWarn : blendColor565(kColorGood, kColorPanel, 0.30f);
        const uint16_t buttonText = kColorText;
        drawAboutUpdateBanner(canvas, button, remoteUpdateReady, armed, buttonFace, buttonText);
    }

    if(!useRoundNavigation()) {
        drawTabs(canvas, layout);
    }
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
            drawBatteryIndicator(canvas, layout, kColorBg);
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
                                      isControllerOtaActionItem(current) ||
                                      (current.action != nullptr && strcmp(current.action, "screen") == 0);
        const bool currentGaugeValuePreview = showValuePreview && isGaugeSetItem(current) && gaugeSetMenuValueKnown();
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
        drawBatteryIndicator(canvas, layout, kColorBg);

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
            if(currentGaugeValuePreview) {
                drawGaugeGapMenuValue(canvas,
                                      cardCx,
                                      static_cast<int16_t>(cardCy + ringInner - 10),
                                      menuValue(current),
                                      heroValueStyle,
                                      RemoteTextStyle::Small,
                                      blendColor565(previewAccent, kColorText, 0.34f),
                                      kColorBg);
            } else {
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
    drawBatteryIndicator(canvas, layout, kColorBg);

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
        if(isGaugeSetItem(items[idx]) && gaugeSetMenuValueKnown()) {
            drawGaugeGapMenuValueTopRight(canvas,
                                          static_cast<int16_t>(layout.menuRows[row].x + layout.menuRows[row].w - layout.gap - 4),
                                          static_cast<int16_t>(layout.menuRows[row].y + layout.gap),
                                          menuValue(items[idx]),
                                          menuValueTextStyle(),
                                          tinyTextStyle(),
                                          textColor,
                                          fill);
        } else {
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
        const bool gaugeEditor = isGaugeSetItem(item);
        const String editorLabel = displayLabelForItem(item);
        const String editorValue = gaugeEditor ? formatValueLabel(item, app.draftValue)
                                               : upperText(formatValueLabel(item, app.draftValue));
        const String editorHint = displayHintForItem(item);
        const int16_t editorLabelWidth = static_cast<int16_t>((layout.editorInnerRadius * 2) + 54);
        const int16_t editorValueWidth = static_cast<int16_t>((layout.editorInnerRadius * 2) + 54);
        const int16_t editorHintWidth = static_cast<int16_t>((layout.editorInnerRadius * 2) + 34);
        const RemoteTextStyle editorLabelStyle = chooseLabelTextStyle(editorLabel, editorLabelWidth);
        const RemoteTextStyle editorValueStyle = chooseValueTextStyle(editorValue, editorValueWidth);
        const RemoteTextStyle editorHintStyle = chooseLabelTextStyle(editorHint, editorHintWidth);

        drawPill(canvas, layout.modeBadge, title, kColorPanel, kColorText);
        drawBatteryIndicator(canvas, layout, kColorBg);

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
            if(gaugeEditor) {
                drawGaugeGapMenuValue(canvas,
                                      layout.editorCx,
                                      layout.editorCy,
                                      app.draftValue,
                                      editorValueStyle,
                                      isLargeCanvas() ? RemoteTextStyle::Medium : RemoteTextStyle::Small,
                                      kColorText,
                                      kColorBg);
            } else {
                canvas.drawCenteredText(fitTextToWidth(editorValue,
                                                       editorValueWidth,
                                                       editorValueStyle,
                                                       12),
                                        layout.editorCx,
                                        layout.editorCy,
                                        editorValueStyle,
                                        kColorText,
                                        kColorBg);
            }
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
    drawBatteryIndicator(canvas, layout, kColorBg);

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
    if(isGaugeSetItem(item)) {
        drawGaugeGapMenuValue(canvas,
                              layout.editorCx,
                              layout.editorCy,
                              app.draftValue,
                              heroTextStyle(),
                              bodyTextStyle(),
                              kColorText,
                              kColorBg);
    } else {
        canvas.drawCenteredText(formatValueLabel(item, app.draftValue),
                                layout.editorCx,
                                layout.editorCy,
                                heroTextStyle(),
                                kColorText,
                                kColorBg);
    }
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

void drawFlashScreen(RemoteCanvas& canvas, const Layout& layout) {
    const uint16_t bg = kColorBg;
    const uint16_t accent = app.flashAccent;
    const uint16_t face = blendColor565(kColorPanel, accent, 0.18f);
    const uint16_t border = blendColor565(accent, kColorText, 0.18f);
    canvas.fillScreen(bg);

    const int16_t cardW = clampI16(static_cast<int16_t>(layout.width - layout.pad * 4), 176, 320);
    const int16_t cardH = clampI16(static_cast<int16_t>(layout.height / 3), 112, 170);
    const Rect card = {
        static_cast<int16_t>((layout.width - cardW) / 2),
        static_cast<int16_t>((layout.height - cardH) / 2),
        cardW,
        cardH
    };

    drawNestCard(canvas, card, face, border, false);

    const int16_t iconY = static_cast<int16_t>(card.y + clampI16(static_cast<int16_t>(card.h / 4), 28, 42));
    const int16_t iconGap = clampI16(static_cast<int16_t>(card.w / 8), 22, 36);
    const int16_t centerX = static_cast<int16_t>(card.x + card.w / 2);
    const int16_t iconSize = clampI16(static_cast<int16_t>(card.h / 6), 16, 24);
    drawNetworkGlyph(canvas,
                     static_cast<int16_t>(centerX - iconGap),
                     iconY,
                     iconSize,
                     NetworkBadgeMode::WifiLan,
                     kColorGood);
    canvas.drawLine(static_cast<int16_t>(centerX - iconGap / 2),
                    iconY,
                    static_cast<int16_t>(centerX + iconGap / 2),
                    iconY,
                    blendColor565(kColorText, accent, 0.36f));
    canvas.drawLine(static_cast<int16_t>(centerX + iconGap / 2 - 4),
                    static_cast<int16_t>(iconY - 4),
                    static_cast<int16_t>(centerX + iconGap / 2),
                    iconY,
                    blendColor565(kColorText, accent, 0.36f));
    canvas.drawLine(static_cast<int16_t>(centerX + iconGap / 2 - 4),
                    static_cast<int16_t>(iconY + 4),
                    static_cast<int16_t>(centerX + iconGap / 2),
                    iconY,
                    blendColor565(kColorText, accent, 0.36f));
    drawNetworkGlyph(canvas,
                     static_cast<int16_t>(centerX + iconGap),
                     iconY,
                     iconSize,
                     NetworkBadgeMode::EspNow,
                     kColorEspNow);

    const String title = upperText(app.flashTitle.length() > 0 ? app.flashTitle : String("WI-FI SYNCED"));
    const String subtitle = app.flashSubtitle.length() > 0 ? app.flashSubtitle : String("Returning to ESP-NOW");
    const int16_t textW = static_cast<int16_t>(card.w - 24);
    const RemoteTextStyle titleStyle = chooseLabelTextStyle(title, textW);
    const RemoteTextStyle subtitleStyle = chooseLabelTextStyle(subtitle, textW);
    canvas.drawCenteredText(fitTextToWidth(title, textW, titleStyle),
                            centerX,
                            static_cast<int16_t>(card.y + card.h / 2 + 12),
                            titleStyle,
                            kColorText,
                            face);
    canvas.drawCenteredText(fitTextToWidth(subtitle, textW, subtitleStyle),
                            centerX,
                            static_cast<int16_t>(card.y + card.h / 2 + 38),
                            subtitleStyle,
                            blendColor565(kColorText, accent, 0.28f),
                            face);
}

uint8_t systemOtaStep(sg98_system_ota::Stage stage) {
    using sg98_system_ota::Stage;
    if(stage == Stage::Connect) return 1;
    if(stage <= Stage::DownloadFiles) return 2;
    if(stage <= Stage::ControllerTransfer) return 3;
    if(stage == Stage::ControllerVerify) return 4;
    return 5;
}

uint8_t systemOtaPercent(uint32_t written, uint32_t total) {
    return total == 0 ? 0 : static_cast<uint8_t>(min<uint32_t>(100, (written * 100ULL) / total));
}

String systemOtaControllerLine() {
    using sg98_system_ota::Stage;
    const Stage stage = g_systemOta.flow.stage;
    if(g_systemOta.flow.controllerVerified) {
        return String("Verified v") + g_systemOta.controllerVersion;
    }
    if(stage == Stage::Connect) return "Checking controller state";
    if(stage == Stage::WifiConnect) return "Waiting while remote joins Wi-Fi";
    if(stage == Stage::DownloadFiles) {
        if(g_systemOta.downloadPart < 2) return "Waiting for controller file";
        return String("Downloading ") + systemOtaPercent(g_systemOta.downloadWritten,
                                                          g_systemOta.downloadTotal) + "%";
    }
    if(stage == Stage::LocalConnect) return "Reconnecting over ESP-NOW";
    if(stage == Stage::LegacyControllerUpdate) return "Downloading compatibility update";
    if(stage == Stage::ControllerTransfer) {
        return String("Transferring ") + systemOtaPercent(g_systemOta.transferOffset,
                                                           g_systemOta.controllerManifest.size) + "%";
    }
    return "Restarting and verifying";
}

String systemOtaRemoteLine() {
    using sg98_system_ota::Stage;
    const Stage stage = g_systemOta.flow.stage;
    if(stage == Stage::WifiConnect) return "Connecting home Wi-Fi";
    if(stage == Stage::DownloadFiles) {
        if(g_systemOta.downloadPart == 1) {
            return String("Downloading ") + systemOtaPercent(g_systemOta.downloadWritten,
                                                              g_systemOta.downloadTotal) + "%";
        }
        return "Remote file verified";
    }
    if(stage == Stage::RemoteInstall) return "Installing staged firmware";
    if(stage > Stage::DownloadFiles) return "Firmware staged and verified";
    return "Waiting for download";
}

void drawSystemOtaProgress(RemoteCanvas& canvas, const Layout& layout) {
    canvas.fillScreen(kColorBg);
    const int16_t centerX = layout.width / 2;
    const int16_t width = min<int16_t>(useRoundNavigation() ? 330 : layout.width - 40,
                                       layout.width - 32);
    const int16_t left = centerX - width / 2;
    const uint8_t step = systemOtaStep(g_systemOta.flow.stage);
    const uint16_t active = blendColor565(kColorAccent, kColorText, 0.12f);
    canvas.drawCenteredText("SYSTEM UPDATE", centerX, useRoundNavigation() ? 62 : 38,
                            RemoteTextStyle::Small, active, kColorBg);
    canvas.drawCenteredText(String("STEP ") + step + " OF 5", centerX,
                            useRoundNavigation() ? 91 : 65,
                            RemoteTextStyle::Tiny, kColorMuted, kColorBg);

    const int16_t barY = useRoundNavigation() ? 116 : 88;
    canvas.fillRoundRect(left, barY, width, 8, 4, kColorPanelHi);
    canvas.fillRoundRect(left, barY, max<int16_t>(8, width * step / 5), 8, 4, active);

    const int16_t controllerY = useRoundNavigation() ? 162 : 126;
    canvas.drawTextTopLeft("CONTROLLER", left, controllerY,
                           RemoteTextStyle::Tiny, kColorMuted, kColorBg);
    canvas.drawTextTopLeft(fitTextToWidthNoTilde(systemOtaControllerLine(), width,
                                                 RemoteTextStyle::Small, 0),
                           left, controllerY + 23, RemoteTextStyle::Small,
                           g_systemOta.flow.controllerVerified ? kColorGood : kColorText, kColorBg);
    String controllerKind = g_systemOta.controllerUi;
    if(!g_systemOta.controllerEnv.isEmpty()) controllerKind += String("  ") + g_systemOta.controllerEnv;
    canvas.drawTextTopLeft(fitTextToWidthNoTilde(controllerKind, width, RemoteTextStyle::Tiny, 0),
                           left, controllerY + 51, RemoteTextStyle::Tiny, kColorMuted, kColorBg);

    const int16_t dividerY = controllerY + 79;
    canvas.drawLine(left, dividerY, left + width, dividerY, kColorPanelHi);
    const int16_t remoteY = dividerY + 20;
    canvas.drawTextTopLeft("REMOTE", left, remoteY,
                           RemoteTextStyle::Tiny, kColorMuted, kColorBg);
    canvas.drawTextTopLeft(fitTextToWidthNoTilde(systemOtaRemoteLine(), width,
                                                 RemoteTextStyle::Small, 0),
                           left, remoteY + 23, RemoteTextStyle::Small, kColorText, kColorBg);
    canvas.drawTextTopLeft(fitTextToWidthNoTilde(String("v") + sg98_version::release() + "  " +
                                                 sg98_version::buildEnv(), width,
                                                 RemoteTextStyle::Tiny, 0),
                           left, remoteY + 51, RemoteTextStyle::Tiny, kColorMuted, kColorBg);

    const int16_t statusY = min<int16_t>(layout.height - 48, remoteY + 96);
    canvas.drawCenteredText(fitTextToWidthNoTilde(String(sg98_system_ota::label(g_systemOta.flow.stage)),
                                                  width, RemoteTextStyle::Small, 0),
                            centerX, statusY, RemoteTextStyle::Small, active, kColorBg);
    canvas.drawCenteredText("Keep both devices powered", centerX, statusY + 25,
                            RemoteTextStyle::Tiny, kColorMuted, kColorBg);
}

void redrawUi() {
    RemoteCanvas& canvas = remote_board_canvas();
    const Layout layout = buildLayout();

    if(systemOtaBusy()) {
        drawSystemOtaProgress(canvas, layout);
    } else if(g_connectionReveal.active) {
        canvas.fillScreen(kColorBg);
        const uint32_t age = millis() - g_connectionReveal.startedAt;
        const float enter = min(1.0f, static_cast<float>(age) / 280.0f);
        const float ease = 1.0f - powf(1.0f - enter, 3.0f);
        const float fade = age > 1200 ? max(0.0f, static_cast<float>(1500 - age) / 300.0f) : ease;
        const int16_t size = static_cast<int16_t>(min<int16_t>(170, layout.width / 2) * (0.86f + 0.14f * ease));
        const int16_t left = (layout.width - size) / 2;
        const int16_t top = static_cast<int16_t>(layout.height / 2 - size / 2 - 25 + 10 * (1.0f - ease));
        if(g_connectionRevealBitmap == nullptr) {
            g_connectionRevealBitmap = static_cast<uint16_t*>(
                heap_caps_malloc(170 * 170 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if(g_connectionRevealBitmap == nullptr) {
            g_connectionReveal.active = false;
            app.redraw = true;
            canvas.drawCenteredText("Grinder connected", layout.width / 2, layout.height / 2,
                                    RemoteTextStyle::Medium, kColorGood, kColorBg);
            canvas.present();
            app.redraw = false;
            return;
        }
        for(int16_t y = 0; y < size; ++y) {
            for(int16_t x = 0; x < size; ++x) {
                const size_t pixel = static_cast<size_t>((y * 124 / size) * 124 + x * 124 / size) * 3;
                const uint8_t alpha = linglong_sg98_cartoon_124_map[pixel + 2];
                const uint16_t color = linglong_sg98_cartoon_124_map[pixel] |
                                       (static_cast<uint16_t>(linglong_sg98_cartoon_124_map[pixel + 1]) << 8);
                g_connectionRevealBitmap[static_cast<size_t>(y) * size + x] =
                    blendColor565(kColorBg, color, fade * alpha / 255.0f);
            }
        }
        canvas.drawRgb565Bitmap(left, top, g_connectionRevealBitmap, size, size);
        const String label = "Grinder connected";
        const auto style = textFitsWidth(label, layout.width - 30, RemoteTextStyle::Medium, 0)
                               ? RemoteTextStyle::Medium : RemoteTextStyle::Small;
        canvas.drawCenteredText(label, layout.width / 2, layout.height / 2 + size / 2 + 5,
                                style, blendColor565(kColorBg, kColorText, fade), kColorBg);
    } else if(app.flashUntil > millis()) {
        drawFlashScreen(canvas, layout);
    } else if(app.dockClockActive) {
        drawDockClock(canvas, layout);
    } else if(app.screen == Screen::Home) {
        drawHome(canvas, layout);
    } else if(app.screen == Screen::Profiler) {
        drawPressureProfiler(canvas, layout);
    } else if(app.screen == Screen::About) {
        drawAboutPage(canvas, layout);
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

bool initEspNow(bool preserveWifi = false);
void deinitEspNowSessionOnly();
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

void serviceMdnsShutdown() {
    if(!g_mdnsStopPending) {
        return;
    }
    if(g_mdnsQuery != nullptr) {
        mdns_result_t* results = nullptr;
        if(!mdns_query_async_get_results(g_mdnsQuery, 0, &results, nullptr)) {
            return;
        }
        mdns_query_async_delete(g_mdnsQuery);
        g_mdnsQuery = nullptr;
        if(results != nullptr) mdns_query_results_free(results);
    }
    if(app.mdnsReady) {
        MDNS.end();
        app.mdnsReady = false;
        Serial.println("[remote_ws] mdns stopped");
    }
    g_mdnsStopPending = false;
}

void stopMdnsClient() {
    // The SDK permits deletion only after an async query finishes.
    g_mdnsStopPending = true;
    serviceMdnsShutdown();
}

void stopWebSocketSession() {
    if(app.wsClientStarted || app.wsConnected) {
        wsClient.disconnect();
    }
    app.wsConnected = false;
    app.wsClientStarted = false;
    app.wsDiscoveryPending = false;
    app.wsStaConnected = false;
    app.wsConnectStartedAt = 0;
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
    app.redraw = true;
}

void ensureMdnsClientReady() {
    serviceMdnsShutdown();
    if(g_mdnsStopPending || app.mdnsReady || !remote_wifi_sta_connected()) {
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
    stopMdnsClient();
    wsClient.begin(app.wsHost.c_str(), app.wsPort, "/");
    wsClient.setReconnectInterval(remote_config::kWebSocketReconnectMs);
    app.wsClientStarted = true;
    app.wsConnectStartedAt = millis();
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
    if(!app.mdnsReady || g_mdnsStopPending) {
        return false;
    }
    if(connectedToControllerAp()) {
        connectWebSocketHost(kControllerApHost, remote_config::kWebSocketPort);
        return true;
    }

    if(g_mdnsQuery != nullptr) {
        mdns_result_t* results = nullptr;
        if(mdns_query_async_get_results(g_mdnsQuery, 0, &results, nullptr)) {
            IPAddress foundIp;
            uint16_t foundPort = 0;
            for(auto* result = results; result != nullptr && foundPort == 0; result = result->next) {
                for(auto* addr = result->addr; addr != nullptr; addr = addr->next) {
                    if(addr->addr.type == MDNS_IP_PROTOCOL_V4 && result->port != 0) {
                        const IPAddress ip(addr->addr.u_addr.ip4.addr);
                        if(validLanHost(ip)) {
                            foundIp = ip;
                            foundPort = result->port;
                            break;
                        }
                    }
                }
            }
            mdns_query_async_delete(g_mdnsQuery);
            g_mdnsQuery = nullptr;
            if(results != nullptr) mdns_query_results_free(results);
            if(foundPort != 0) {
                connectWebSocketHost(foundIp.toString(), foundPort);
                app.wsSubnetProbeCursor = 0;
                return true;
            }
        }
    }

    const uint32_t now = millis();
    if(g_mdnsQuery == nullptr &&
       (force || app.lastMdnsQueryAt == 0 ||
        static_cast<uint32_t>(now - app.lastMdnsQueryAt) >= remote_config::kMdnsQueryMs)) {
        app.lastMdnsQueryAt = now;
        g_mdnsQuery = mdns_query_async_new(nullptr, "_sg98", "_tcp", MDNS_TYPE_PTR, 3000, 4, nullptr);
    }

    if(static_cast<uint32_t>(now - g_lastSubnetProbeAt) >= kWebSocketSubnetProbeIntervalMs) {
        g_lastSubnetProbeAt = now;
        IPAddress ip;
        if(probeControllerSubnet(&ip)) {
            connectWebSocketHost(ip.toString(), remote_config::kWebSocketPort);
            return true;
        }
    }
    return false;
}

void requestWebSocketDiscovery(bool resetSession) {
    if(resetSession) {
        stopMdnsClient();
        if(app.wsClientStarted || app.wsConnected) {
            wsClient.disconnect();
        }
        app.wsConnected = false;
        app.wsClientStarted = false;
        app.wsConnectStartedAt = 0;
        app.wsHost = "";
        app.wsPort = remote_config::kWebSocketPort;
        app.wsEndpointLabel = "WebSocket scan";
        app.wsSubnetProbeCursor = 0;
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
    peer.channel = 0;
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
        if(menuNum > 0 && menuNum <= kControllerMenuMax && app.lastMenuRxAt[menuNum] != startedMenuAt) {
            return true;
        }
        delay(1);
    }
    return app.lastAckAt != startedAckAt ||
           app.lastStatusAt != startedStatusAt ||
           (menuNum > 0 && menuNum <= kControllerMenuMax && app.lastMenuRxAt[menuNum] != startedMenuAt);
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
    if(rc != ESP_OK) ++g_espNowTxEnqueueFailed;
    return rc == ESP_OK;
}

bool sendEspNowBytes(const uint8_t* mac, const uint8_t* data, size_t length) {
    if(!app.espNowReady || mac == nullptr || data == nullptr || length == 0 || length > 250) {
        return false;
    }
    if(!ensureEspNowPeer(mac)) return false;
    app.sendComplete = false;
    app.sendOk = false;
    const esp_err_t err = esp_now_send(mac, data, length);
    if(err != ESP_OK) ++g_espNowTxEnqueueFailed;
    return err == ESP_OK;
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

    const uint8_t lockedChannel = activeEspNowPeerChannel();
    if(lockedChannel != 0 && app.espNowChannel != lockedChannel) {
        (void)setEspNowChannel(lockedChannel);
    } else if(force && (!hasActiveEspNowPeer() || !isLinkOnline())) {
        (void)setEspNowChannel(remote_config::kEspNowChannel);
    } else if((app.screen == Screen::EspNowPeers || !hasActiveEspNowPeer() || !isLinkOnline()) &&
              app.lastEspNowDiscoveryAt != 0) {
        (void)setEspNowChannel(nextEspNowDiscoveryChannel());
    }
    app.lastEspNowDiscoveryAt = now;
    sendEspNowFrame(kEspNowBroadcastMac, sg98_protocol::discoverRequest());
}

void requestEspNowRecoveryHint(bool force) {
    if(!app.espNowReady) {
        return;
    }

    const uint32_t now = millis();
    if(!force &&
       app.lastWebFallbackRecoveryHintAt != 0 &&
       (now - app.lastWebFallbackRecoveryHintAt) < kWebFallbackRecoveryHintMs) {
        return;
    }

    bool sent = false;
    const uint8_t* peerMac = activeEspNowPeerMac();
    if(peerMac != nullptr) {
        sent = sendEspNowFrame(peerMac, kEspNowRecoveryHintCommand) || sent;
    }
    sent = sendEspNowFrame(kEspNowBroadcastMac, kEspNowRecoveryHintCommand) || sent;
    if(sent) {
        app.lastWebFallbackRecoveryHintAt = now;
        if(app.preserveWifiForEspNowRecovery && app.webFallbackWifiReleaseAt == 0) {
            app.webFallbackWifiReleaseAt = now + kWebFallbackWifiReleaseDelayMs;
        }
    }
}

void clearOfflineEspNowLockForSearch(uint32_t now) {
    if(g_systemOta.flow.stage == sg98_system_ota::Stage::ControllerTransfer) {
        return;
    }
    if(!app.espNowPeerConfigured || isLinkOnline() ||
       (now - app.lastTransportSwitchAt) < kStaleEspNowLockClearMs) {
        return;
    }

    if(webFallbackEspNowActive(now)) {
        return;
    }

    clearLockedEspNowPeer();
    clearPollSchedule();
    setToast("Searching controller", 900);
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

void requestControllerEnvironment(bool force) {
    const uint32_t now = millis();
    if(!force && app.lastControllerEnvironmentRequestAt != 0 &&
       (now - app.lastControllerEnvironmentRequestAt) < kControllerEnvironmentRetryMs) {
        return;
    }
    if(sendRaw(sg98_protocol::kCmdGetEnvironment)) {
        app.lastControllerEnvironmentRequestAt = now;
    }
}

void serviceSafetyHeartbeat() {
    if(!isRunningState()) {
        return;
    }

    const uint32_t now = millis();
    if((now - app.lastSafetyHeartbeatAt) < remote_config::kSafetyHeartbeatMs) {
        return;
    }

    (void)sendRaw(sg98_protocol::kCmdHeartbeat);
    app.lastSafetyHeartbeatAt = now;
}

void requestMenu(uint8_t menuNum) {
    if(menuNum >= 1 && menuNum <= kControllerMenuMax && sendRaw(sg98_protocol::getMenu(menuNum))) {
        app.lastMenuPollAt[menuNum] = millis();
    }
}

void requestAllMenus() {
    for(uint8_t menuNum = 1; menuNum <= kControllerMenuMax; ++menuNum) {
        requestMenu(menuNum);
    }
}

// Rehydrate the remote from the controller whenever a transport comes up so
// stale locally cached menu values do not survive between sessions.
void requestFullControllerRefresh() {
    requestStatus();
    requestAllMenus();
    requestControllerEnvironment(true);
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
            requestIfDue(5);
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
           pending.menuNum > 0 && pending.menuNum <= kControllerMenuMax &&
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
            if(pending.kind == PendingActionKind::Stop) {
                return statusStateIsStopped(app.controllerStatus.state) ||
                       app.controllerStatus.state == "PURGING";
            }
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

bool pendingActionRejectedByStatus(const PendingAction& pending,
                                   const sg98_protocol::StatusFrame& status) {
    if(pending.kind == PendingActionKind::Start && pendingActionSent(pending)) {
        if(status.state == "MOTOR_SELECT") {
            return true;
        }
        return !statusStateIsRunning(status.state) && statusMotorUnavailable(status);
    }
    return false;
}

String pendingActionRejectedMessage(const PendingAction& pending,
                                    const sg98_protocol::StatusFrame& status) {
    if(pending.kind == PendingActionKind::Start) {
        if(status.state == "MOTOR_SELECT") {
            return "Select drive first";
        }
        if(status.motor == "FAULT") {
            return "Start failed: motor fault";
        }
        return "Start failed: motor offline";
    }
    return "Action rejected";
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
            if(itemIdx == RADIOMODE) {
                return safeUserRadioMode(requestedValue);
            }
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
        case PendingActionKind::DriveRt:
        case PendingActionKind::DriveKrs:
        case PendingActionKind::DriveXb130:
        case PendingActionKind::DriveSew:
        case PendingActionKind::DriveTse: return "drive";
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
       !pendingActionRejectedByStatus(app.pendingAction, *status) &&
       !pendingActionSatisfied(app.pendingAction)) {
        status->state = String(app.pendingAction.expectedText);
    }
}

void applyPendingUiOverridesToMenu(uint8_t menuNum) {
    if(menuNum == 0 || menuNum > kControllerMenuMax) {
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

void rejectPendingActionFromStatusIfNeeded(const sg98_protocol::StatusFrame& status) {
    if(!pendingActionRejectedByStatus(app.pendingAction, status)) {
        return;
    }
    const String message = pendingActionRejectedMessage(app.pendingAction, status);
    clearPendingAction();
    setToast(message, 1600);
}

void servicePendingMenuPolls() {
    bool requestedMenu[kControllerMenuSlots] = {};
    const uint32_t now = millis();
    for(const PendingMenuWrite& pending : app.pendingMenuWrites) {
        if(!pending.active || pending.menuNum == 0 || pending.menuNum > kControllerMenuMax ||
           requestedMenu[pending.menuNum]) {
            continue;
        }
        if((now - app.lastMenuPollAt[pending.menuNum]) >= remote_config::kPendingMenuPollMs) {
            requestMenu(pending.menuNum);
            requestedMenu[pending.menuNum] = true;
        }
    }
}

void servicePendingControllerSync() {
    if(systemOtaBusy()) return;
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

bool wifiRecoveryCanFallBackToEspNow() {
    if(!remote_wifi_has_saved_network()) {
        return false;
    }
    if(remote_wifi_sta_connected()) {
        return true;
    }
    return remote_wifi_sta_attempt_count() >= kWifiFallbackMinStaAttempts;
}

String wifiRecoveryFallbackDetail() {
    if(remote_wifi_sta_connected()) {
        return "Controller not found on Wi-Fi";
    }
    return String("Tried home Wi-Fi ") + String(static_cast<unsigned>(kWifiFallbackMinStaAttempts)) + "x";
}

bool webFallbackEspNowActive(uint32_t now = millis()) {
    if(app.webFallbackEspNowUntil == 0) {
        return false;
    }
    if(static_cast<int32_t>(now - app.webFallbackEspNowUntil) >= 0) {
        app.webFallbackEspNowUntil = 0;
        app.preserveWifiForEspNowRecovery = false;
        app.lastWebFallbackRecoveryHintAt = 0;
        app.webFallbackWifiReleaseAt = 0;
        return false;
    }
    return true;
}

bool webFallbackWifiReleaseDue(uint32_t now) {
    return app.webFallbackWifiReleaseAt != 0 &&
           static_cast<int32_t>(now - app.webFallbackWifiReleaseAt) >= 0;
}

bool webFallbackShouldPreserveWifi(uint32_t now) {
    return app.preserveWifiForEspNowRecovery &&
           webFallbackEspNowActive(now) &&
           !webFallbackWifiReleaseDue(now) &&
           remote_wifi_is_active() &&
           remote_wifi_sta_connected();
}

void requestWebFallbackWifiRelease(uint32_t now) {
    app.webFallbackWifiReleaseAt = now;
    app.lastEspNowDiscoveryAt = 0;
    clearPollSchedule();
}

bool webFallbackEspNowConfirmed() {
    return app.controllerMenuSynced[1] &&
           sanitizeControllerRadioMode(app.controllerMenuValues[1][RADIOMODE]) == kControllerRadioEspNow;
}

bool pendingRadioModeWriteActive(int32_t* targetOut = nullptr) {
    for(const PendingMenuWrite& pending : app.pendingMenuWrites) {
        if(pending.active && pending.menuNum == 1 && pending.itemIdx == RADIOMODE) {
            if(targetOut != nullptr) {
                *targetOut = pending.value;
            }
            return true;
        }
    }
    return false;
}

bool requestControllerRadioMode(int32_t mode, bool replacePending = false) {
    mode = sanitizeControllerRadioMode(mode);
    int32_t pendingTarget = 0;
    if(!replacePending && pendingRadioModeWriteActive(&pendingTarget)) {
        return pendingTarget == mode;
    }

    const uint32_t now = millis();
    if(!replacePending && now - app.lastAutoRadioRequestAt < kAutoRadioRequestMs) {
        return false;
    }

    app.menuValues[1][RADIOMODE] = mode;
    app.radioModeKnown = true;
    PendingMenuWrite* pending = stagePendingMenuWrite(1, RADIOMODE, mode);
    if(pending == nullptr) {
        return false;
    }

    app.lastAutoRadioRequestAt = now;
    (void)trySendPendingMenuWrite(pending, true);
    app.lastMenuPollAt[1] = 0;
    markPersistentStateDirty();
    return true;
}

void notifyAutoEspNowReturn(bool synced, const String& fallbackDetail = String()) {
    if(app.autoEspNowReturnNotified) {
        return;
    }
    app.autoEspNowReturnNotified = true;
    const String title = synced ? String("Wi-Fi synced") : String("Wi-Fi fallback");
    const String subtitle = synced ? String("Config restored to ESP-NOW")
                                   : (fallbackDetail.length() > 0 ? fallbackDetail
                                                                  : String("Returning to ESP-NOW"));
    holdNetworkCurveStatus(synced ? String("WIFI SYNCED") : String("WIFI FALLBACK"),
                           kWifiReturnCurveHoldMs);
    showFlashScreen(title,
                    subtitle,
                    kAutoEspNowReturnFlashMs,
                    kColorEspNow);
}

void serviceEspNowFirstPolicy() {
    if(systemOtaBusy()) return;
    if(app.currentTransport == LinkTransport::WebSocket) {
        const bool websocketOverride = app.transportOverrideActive;
        if(websocketOverride) {
            return;
        }
        if(app.wsConnected && app.radioModeKnown) {
            const int32_t mode = app.controllerMenuSynced[1]
                                     ? sanitizeControllerRadioMode(app.controllerMenuValues[1][RADIOMODE])
                                     : sanitizeControllerRadioMode(app.menuValues[1][RADIOMODE]);
            if(mode == kControllerRadioEspNow) {
                (void)requestControllerRadioMode(kControllerRadioEspNow, true);
                notifyAutoEspNowReturn(true);
                scheduleTransportSwitch(LinkTransport::EspNow, remote_config::kTransportSwitchDelayMs);
                return;
            }
        }

        if(!app.wsConnected &&
           app.webRecoveryStartedAt != 0 &&
           millis() - app.webRecoveryStartedAt >= kWifiRecoveryWindowMs) {
            if(!wifiRecoveryCanFallBackToEspNow()) {
                app.redraw = true;
                return;
            }
            app.webFallbackEspNowUntil = millis() + kWebFallbackEspNowHoldMs;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
            app.preserveWifiForEspNowRecovery = remote_wifi_sta_connected();
            (void)requestControllerRadioMode(kControllerRadioEspNow, true);
            notifyAutoEspNowReturn(false, wifiRecoveryFallbackDetail());
            switchTransportNow(LinkTransport::EspNow);
            requestEspNowDiscovery(true);
            requestEspNowRecoveryHint(true);
            requestFullControllerRefresh();
        }
        return;
    }

    if(app.currentTransport != LinkTransport::EspNow ||
       !isLinkOnline() ||
       !hasActiveEspNowPeer() ||
       !app.radioModeKnown) {
        return;
    }

    if(webFallbackEspNowActive()) {
        if(webFallbackEspNowConfirmed()) {
            app.webFallbackEspNowUntil = 0;
            app.preserveWifiForEspNowRecovery = false;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
        } else {
            (void)requestControllerRadioMode(kControllerRadioEspNow);
            return;
        }
    }

    const int32_t mode = sanitizeControllerRadioMode(app.menuValues[1][RADIOMODE]);
    if(transportFromRadioMode(mode) == LinkTransport::WebSocket) {
        scheduleTransportSwitch(LinkTransport::WebSocket, remote_config::kTransportSwitchDelayMs);
    }
}

void switchTransportNow(LinkTransport transport) {
    ++g_transportGeneration;
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
        if(transport == LinkTransport::EspNow &&
           remote_wifi_is_active() &&
           !webFallbackShouldPreserveWifi(millis())) {
            deinitEspNowSessionOnly();
            remote_wifi_stop();
            (void)initEspNow(false);
            clearPollSchedule();
        }
        app.redraw = true;
        return;
    }

    app.lastLinkActivityAt = 0;

    if(transport == LinkTransport::WebSocket) {
        stopEspNowSession();
        if(!remote_wifi_is_active()) {
            remote_wifi_start();
        }
        app.webRecoveryStartedAt = millis();
        app.wsStaConnected = remote_wifi_sta_connected();
        app.wsDiscoveryPending = remote_wifi_has_saved_network();
        if(!remote_wifi_has_saved_network()) {
            app.toastUntil = 0;
        }
    } else if(transport == LinkTransport::EspNow) {
        stopWebSocketSession();
        const bool preserveWifi =
            app.preserveWifiForEspNowRecovery &&
            webFallbackEspNowActive() &&
            remote_wifi_is_active() &&
            remote_wifi_sta_connected();
        if(remote_wifi_is_active() && !preserveWifi) {
            deinitEspNowSessionOnly();
            remote_wifi_stop();
        }
        const uint8_t peerChannel = activeEspNowPeerChannel();
        if(preserveWifi) {
            const uint8_t wifiChannel = static_cast<uint8_t>(WiFi.channel());
            if(wifiChannel >= kEspNowMinChannel && wifiChannel <= kEspNowMaxChannel) {
                app.espNowChannel = wifiChannel;
            }
        } else if(peerChannel != 0) {
            app.espNowChannel = peerChannel;
        } else if(!hasActiveEspNowPeer() || !isLinkOnline()) {
            app.espNowChannel = remote_config::kEspNowChannel;
            app.lastEspNowDiscoveryAt = 0;
        }
        app.webRecoveryStartedAt = 0;
        app.wsDiscoveryPending = false;
        app.wsStaConnected = false;
        if(initEspNow(preserveWifi)) {
            setToast("ESP-NOW mode", 1200);
        }
    }

    app.currentTransport = transport;
    app.lastTransportSwitchAt = millis();
    clearPollSchedule();
    markPersistentStateDirty();
    app.redraw = true;
}

void applyPendingTransportSwitch() {
    if(app.pendingTransport == LinkTransport::None || millis() < app.pendingTransportAt) {
        return;
    }

    switchTransportNow(app.pendingTransport);
}

LinkTransport alternateTransport() {
    if(webFallbackEspNowActive()) {
        return LinkTransport::EspNow;
    }

    if(app.transportOverrideActive) {
        return app.currentTransport;
    }

    if(app.radioModeKnown) {
        const LinkTransport expected = transportFromRadioMode(app.menuValues[1][RADIOMODE]);
        if(expected == LinkTransport::EspNow) {
            if(app.currentTransport == LinkTransport::WebSocket && app.wsConnected) {
                return LinkTransport::WebSocket;
            }
            return LinkTransport::EspNow;
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
        if(app.screen == Screen::EspNowPeers) {
            return app.currentTransport;
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
    reconcilePendingControllerSync();
    rejectPendingActionFromStatusIfNeeded(status);
    sg98_protocol::StatusFrame displayStatus = status;
    applyPendingUiOverridesToStatusFrame(&displayStatus);
    const bool valuesChanged = !hadStatus || !statusFrameEqual(app.status, displayStatus);
    const bool gaugeChanged =
        !hadStatus ||
        statusHasEnabledGauge(previous) != statusHasEnabledGauge(displayStatus) ||
        previous.burrGapKnown != displayStatus.burrGapKnown ||
        previous.burrGapLive != displayStatus.burrGapLive ||
        previous.burrGapUm != displayStatus.burrGapUm;
    const bool controllerOtaReadyReported =
        displayStatus.controllerOtaReadyFieldsPresent &&
        displayStatus.controllerOtaReady;
    const bool controllerOtaBecameReady =
        controllerOtaReadyReported &&
        (!hadStatus || !previous.controllerOtaReady);
    app.status = displayStatus;
    app.hasStatus = true;
    const uint32_t now = millis();
    app.lastLinkActivityAt = now;
    app.lastStatusAt = now;
    if(statusHasEnabledGauge(displayStatus) && gaugeChanged) {
        app.lastGaugeStatusChangeAt = now;
    }
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
    if(!hadStatus || previous.drive != app.status.drive) {
        sanitizeSelections();
        app.redraw = true;
    }
    if(shouldRedrawForStatusFrame(previous, app.status, hadStatus, valuesChanged)) {
        app.deferredHomeStatusRedraw = false;
        app.redraw = true;
    } else if(valuesChanged && useRoundNavigation() && app.screen == Screen::Home && roundUiInteractionActive()) {
        app.deferredHomeStatusRedraw = true;
    }
    if(controllerOtaReadyReported && app.controllerOtaActionBusy) {
        clearControllerOtaBusy();
        clearControllerOtaDownloadArm();
        setToast("Controller OTA ready", 2200);
    } else if(controllerOtaBecameReady) {
        setToast("Controller OTA ready", 2200);
    }
    requestFullControllerRefreshOnLinkOnline(wasOnline);
}

void consumeMenuFrame(const sg98_protocol::MenuFrame& frame) {
    if(frame.menuNum == 0 || frame.menuNum > kControllerMenuMax) {
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
            const bool connectedWebSocketRecovery =
                app.currentTransport == LinkTransport::WebSocket &&
                app.wsConnected &&
                expected == LinkTransport::EspNow;
            if(!connectedWebSocketRecovery) {
                scheduleTransportSwitch(expected);
            }
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

bool upsertDiscoveredEspNowPeer(const sg98_protocol::PeerFrame& frame) {
    uint8_t mac[6] = {};
    if(!parseMacAddress(frame.mac, mac) || macAddressIsZero(mac)) {
        return false;
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
    const bool staleLockedPeer = app.espNowPeerConfigured &&
                                 !lockedEspNowPeerMatches(peer.mac) &&
                                 !isLinkOnline();
    const bool shouldAutoLock = !hasActiveEspNowPeer() || staleLockedPeer;
    const bool lockedPeerMatches = lockedEspNowPeerMatches(peer.mac);
    if(lockedPeerMatches || shouldAutoLock) {
        (void)setEspNowChannel(peer.channel);
    }
    if(shouldAutoLock) {
        setLockedEspNowPeer(peer.mac);
        if(app.espNowReady) {
            ensureEspNowPeer(peer.mac);
        }
        clearPollSchedule();
        markPersistentStateDirty();
        setToast(String(staleLockedPeer ? "Relocked " : "Locked ") + espNowPeerName(peer), 1000);
    }
    sanitizeSelections();
    app.redraw = app.redraw || app.screen == Screen::EspNowPeers;
    return lockedPeerMatches || shouldAutoLock;
}

void consumePeerFrame(const sg98_protocol::PeerFrame& frame) {
    clearRecoveredSendFailureToast();
    if(upsertDiscoveredEspNowPeer(frame)) {
        app.lastLinkActivityAt = millis();
    }
}

void consumeEnvironmentFrame(const sg98_protocol::EnvironmentFrame& frame) {
    clearRecoveredSendFailureToast();
    const uint32_t now = millis();
    app.lastLinkActivityAt = now;

    bool changed = false;
    struct tm existingTime = {};
    if(frame.timeValid && frame.unixTime >= kValidUnixTime && !localClockTime(&existingTime)) {
        ensureDockClockTimezone();
        timeval tv = {};
        tv.tv_sec = static_cast<time_t>(frame.unixTime);
        settimeofday(&tv, nullptr);
        app.dockClockTimeConfigured = true;
        app.dockClockLastNtpSyncAt = now;
        changed = true;
    }

    if(frame.weatherValid) {
        app.dockWeather.valid = true;
        app.dockWeather.temperatureF = frame.temperatureF;
        app.dockWeather.weatherCode = frame.weatherCode;
        app.dockWeather.precipitationPercent = frame.precipitationPercent;
        app.dockWeather.location = frame.location;
        app.dockWeather.updatedAt = now;
        app.dockClockLastWeatherFetchAt = now;
        changed = true;
    }

    if(changed) {
        app.redraw = true;
    }
}

bool handleControllerOtaAck(const sg98_protocol::AckFrame& ackFrame) {
    if(!app.controllerOtaActionBusy) {
        return false;
    }

    String message = ackFrame.message;
    String lower = message;
    lower.toLowerCase();
    if(lower == "wifi saved") {
        updateControllerOtaProgressLabel("Wi-Fi saved");
        return true;
    }
    if(lower == "wifi not saved") {
        updateControllerOtaProgressLabel("Wi-Fi not saved");
        return true;
    }
    if(lower == "env synced" || lower == "env ok") {
        updateControllerOtaProgressLabel("Environment synced");
        return true;
    }

    clearControllerOtaBusy();
    if(!ackFrame.ok) {
        clearControllerOtaDownloadArm();
        setToast(String("OTA ERR: ") + message, 2600);
        return true;
    }

    if(lower.startsWith("update available")) {
        armControllerOtaDownload();
        setToast("Update found: press OTA again", kControllerOtaDownloadConfirmMs);
        return true;
    }

    if(lower.startsWith("download ready")) {
        clearControllerOtaDownloadArm();
        app.lastStatusPollAt = 0;
        setToast("Controller OTA ready", 2400);
        return true;
    }

    if(lower == "current" || lower.startsWith("current ")) {
        clearControllerOtaDownloadArm();
        setToast("Controller is current", 1800);
        return true;
    }

    if(lower.startsWith("boot=") || lower.indexOf("reboot queued") >= 0) {
        clearControllerOtaDownloadArm();
        showFlashScreen("CONTROLLER OTA",
                        "Controller rebooting",
                        kControllerOtaRebootFlashMs,
                        kColorWarn);
        app.lastStatusPollAt = 0;
        setToast("Controller rebooting", 2400);
        return true;
    }

    clearControllerOtaDownloadArm();
    setToast(message.length() > 0 ? message : String("Controller OTA done"), 2200);
    return true;
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

    sg98_protocol::EnvironmentFrame environmentFrame;
    if(sg98_protocol::parseEnvironmentFrame(frame, &environmentFrame)) {
        consumeEnvironmentFrame(environmentFrame);
        return;
    }

    sg98_protocol::WifiConfigFrame wifiFrame;
    if(sg98_protocol::parseWifiConfigFrame(frame, &wifiFrame)) {
        clearRecoveredSendFailureToast();
        app.lastLinkActivityAt = millis();
        remote_wifi_apply_network_settings(wifiFrame.ssid, wifiFrame.password, true);
        setToast(String("Wi-Fi synced: ") + wifiFrame.ssid, 1600);
        app.redraw = true;
        return;
    }

    sg98_protocol::AckFrame ackFrame;
    if(sg98_protocol::parseAckFrame(frame, &ackFrame)) {
        const bool wasOnline = isLinkOnline();
        app.lastLinkActivityAt = millis();
        app.lastAckAt = millis();
        if(handleSystemOtaAck(ackFrame)) return;
        if(ackFrame.ok) {
            clearRecoveredSendFailureToast();
        } else if(app.pendingAction.kind != PendingActionKind::None) {
            clearPendingAction();
            if(app.hasControllerStatus) {
                app.status = app.controllerStatus;
                app.hasStatus = true;
            }
            app.lastStatusPollAt = 0;
            app.redraw = true;
        }
        requestFullControllerRefreshOnLinkOnline(wasOnline);
        if(ackFrame.ok && ackFrame.message == "recovery requested") {
            requestWebFallbackWifiRelease(millis());
        }
        if(ackFrame.ok && ackFrame.message == "ok") {
            return;
        }
        if(handleControllerOtaAck(ackFrame)) {
            return;
        }
        setToast(ackFrame.ok ? ackFrame.message : String("ERR: ") + ackFrame.message);
    }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            app.wsConnected = true;
            app.wsConnectStartedAt = 0;
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
    if(status == ESP_NOW_SEND_SUCCESS) {
        ++g_espNowTxDelivered;
    } else {
        ++g_espNowTxFailed;
    }
    app.sendOk = (status == ESP_NOW_SEND_SUCCESS);
    app.sendComplete = true;
}

void onEspNowReceive(const esp_now_recv_info_t*, const uint8_t* data, int len) {
    if(data == nullptr || len <= 0) {
        return;
    }

    if(sg98_ota_transport::isFrame(data, static_cast<size_t>(len))) {
        sg98_ota_transport::Header header = {};
        memcpy(&header, data, sizeof(header));
        if(header.type == sg98_ota_transport::Type::Ack &&
           len >= static_cast<int>(sizeof(sg98_ota_transport::AckFrame))) {
            portENTER_CRITICAL_ISR(&g_systemOtaAckMux);
            memcpy(&g_systemOtaAck, data, sizeof(g_systemOtaAck));
            g_systemOtaAckPending = true;
            portEXIT_CRITICAL_ISR(&g_systemOtaAckMux);
            remote_runtime_notify_input();
        }
        return;
    }
    size_t textLength = static_cast<size_t>(len);
    while(textLength > 0 && data[textLength - 1] == '\0') {
        --textLength;
    }
    if(textLength > 0) {
        consumeProtocolFrame(String(reinterpret_cast<const char*>(data),
                                    static_cast<unsigned int>(textLength)));
    }
}

bool initEspNow(bool preserveWifi) {
    if(app.espNowReady) {
        return true;
    }

    WiFi.setSleep(!remote_config::kDisableWifiModemSleepForTesting);

    if(preserveWifi && remote_wifi_is_active()) {
        const uint8_t wifiChannel = static_cast<uint8_t>(WiFi.channel());
        if(wifiChannel >= kEspNowMinChannel && wifiChannel <= kEspNowMaxChannel) {
            app.espNowChannel = wifiChannel;
        }
    } else {
        WiFi.mode(WIFI_STA);

        const uint8_t peerChannel = activeEspNowPeerChannel();
        if(peerChannel != 0) {
            app.espNowChannel = peerChannel;
        } else if(!hasActiveEspNowPeer() || !isLinkOnline()) {
            app.espNowChannel = remote_config::kEspNowChannel;
        }
        const uint8_t channel = normalizeEspNowChannel(app.espNowChannel);
        if(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            setToast("Channel setup failed");
            return false;
        }
        app.espNowChannel = channel;
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
    if(!app.sendOk && !pendingControllerSyncActive() && !systemOtaBusy() && !controllerOtaActionBusy()) {
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

    const auto otaStage = g_systemOta.flow.stage;
    const bool otaWifiOnly = systemOtaBusy() &&
        (otaStage == sg98_system_ota::Stage::WifiConnect ||
         otaStage == sg98_system_ota::Stage::DownloadFiles);

    if(app.currentTransport == LinkTransport::WebSocket && otaWifiOnly) {
        if(app.wsClientStarted || app.wsConnected || app.wsDiscoveryPending || app.mdnsReady || g_mdnsQuery != nullptr) {
            stopWebSocketSession();
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
            app.redraw = true;
        }
    } else if(app.currentTransport == LinkTransport::WebSocket) {
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
            app.redraw = true;
            if(staConnected) {
                app.wsDiscoveryPending = true;
            }
        }
        if(staConnected) {
            ensureMdnsClientReady();
            if(app.wsClientStarted &&
               !app.wsConnected &&
               app.wsConnectStartedAt != 0 &&
               (now - app.wsConnectStartedAt) >= kWebSocketCandidateRetryMs) {
                wsClient.disconnect();
                app.wsClientStarted = false;
                app.wsConnectStartedAt = 0;
                app.wsHost = "";
                app.wsEndpointLabel = "WebSocket scan";
                app.wsDiscoveryPending = true;
                app.lastMdnsQueryAt = 0;
            }
            if(!app.wsClientStarted && !app.wsHost.isEmpty()) {
                connectWebSocketHost(app.wsHost, app.wsPort);
            }
            if(app.wsClientStarted &&
               (now - app.lastWebSocketServiceAt) >= currentWebSocketLoopIntervalMs()) {
                wsClient.loop();
                app.lastWebSocketServiceAt = now;
            }
            if(app.wsDiscoveryPending) {
                const bool found = discoverControllerService(false);
                app.wsDiscoveryPending = !found && !app.wsClientStarted;
            }
        } else {
            if(app.wsClientStarted || app.wsConnected) {
                wsClient.disconnect();
                app.wsClientStarted = false;
                app.wsConnected = false;
                app.wsConnectStartedAt = 0;
            }
            stopMdnsClient();
        }
    } else if(app.currentTransport == LinkTransport::EspNow) {
        stopWebSocketSession();
        if(app.preserveWifiForEspNowRecovery && webFallbackWifiReleaseDue(now)) {
            app.preserveWifiForEspNowRecovery = false;
            app.webFallbackWifiReleaseAt = 0;
            app.lastEspNowDiscoveryAt = 0;
        }
        const bool preserveWifi = webFallbackShouldPreserveWifi(now);
        if(remote_wifi_is_active() && preserveWifi) {
            if((now - app.lastWifiServiceAt) >= remote_config::kWifiServiceMs) {
                remote_wifi_poll();
                app.lastWifiServiceAt = now;
            }
        } else if(remote_wifi_is_active()) {
            deinitEspNowSessionOnly();
            remote_wifi_stop();
        }
        initEspNow(preserveWifi);
        if(webFallbackEspNowActive(now)) {
            requestEspNowRecoveryHint(false);
        }
        if(app.preserveWifiForEspNowRecovery &&
           (!webFallbackEspNowActive(now) || (isLinkOnline() && webFallbackEspNowConfirmed()))) {
            app.webFallbackEspNowUntil = 0;
            app.preserveWifiForEspNowRecovery = false;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
        }
        clearOfflineEspNowLockForSearch(now);
        if(g_systemOta.flow.stage != sg98_system_ota::Stage::ControllerTransfer &&
           (app.screen == Screen::EspNowPeers || !hasActiveEspNowPeer() || !isLinkOnline())) {
            requestEspNowDiscovery(false);
        }
    }

    if(!systemOtaBusy() && !isLinkOnline() && (millis() - app.lastTransportSwitchAt) >= remote_config::kTransportSearchWindowMs) {
        const LinkTransport fallback = alternateTransport();
        if(fallback != app.currentTransport) {
            if(app.currentTransport == LinkTransport::WebSocket &&
               fallback == LinkTransport::EspNow) {
                if(!wifiRecoveryCanFallBackToEspNow()) {
                    app.lastTransportSwitchAt = millis();
                    app.redraw = true;
                    return;
                }
                notifyAutoEspNowReturn(false, wifiRecoveryFallbackDetail());
            }
            switchTransportNow(fallback);
        } else {
            app.lastTransportSwitchAt = millis();
        }
    }
}

void syncPolling() {
    if(systemOtaBusy()) return;
    if(millis() - app.lastStatusPollAt >= currentStatusPollIntervalMs()) {
        requestStatus();
        app.lastStatusPollAt = millis();
    }

    servicePendingMenuPolls();

    if(millis() - app.lastBackgroundMenuPollAt >= remote_config::kBackgroundMenuPollMs) {
        for(uint8_t menuNum = 1; menuNum <= kControllerMenuMax; ++menuNum) {
            requestMenu(menuNum);
        }
        app.lastBackgroundMenuPollAt = millis();
    }

    if(!roundUiInteractionActive()) {
        requestVisibleMenuIfNeeded(false);
    }
}

bool applyMenuValue(const SettingItem& item, int32_t value, bool allowGaugeSetWrite = false) {
    if(!settingItemVisible(item)) {
        return false;
    }

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
    if(isGaugeSetItem(item) && !allowGaugeSetWrite) {
        requestMenu(1);
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
        const LinkTransport requestedTransport = transportFromRadioMode(normalizedValue);
        app.transportOverrideActive = requestedTransport == LinkTransport::WebSocket;
        if(requestedTransport == LinkTransport::WebSocket) {
            app.autoEspNowReturnNotified = false;
            app.webFallbackEspNowUntil = 0;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
            app.preserveWifiForEspNowRecovery = false;
        }
        scheduleTransportSwitch(requestedTransport, remote_config::kTransportSwitchDelayMs);
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
    (void)setEspNowChannel(peer.channel);
    if(app.espNowReady) {
        ensureEspNowPeer(peer.mac);
    }
    clearPollSchedule();
    requestFullControllerRefresh();
    markPersistentStateDirty();
    setToast(String("Locked ") + espNowPeerName(peer), 1200);
}

bool sendControllerWifiConfigIfAvailable() {
    if(!remote_wifi_has_saved_network()) {
        return false;
    }
    const String ssid = remote_wifi_sta_ssid();
    if(ssid.isEmpty()) {
        return false;
    }
    return sendRaw(sg98_protocol::wifiConfig(ssid, remote_wifi_sta_password()));
}

void beginControllerOtaBusy(const String& title, const String& subtitle, uint32_t durationMs) {
    const uint32_t now = millis();
    app.controllerOtaActionBusy = true;
    app.controllerOtaActionStartedAt = now;
    app.controllerOtaActionUntil = now + durationMs;
    app.lastControllerOtaProgressAt = now;
    app.controllerOtaProgressLabel = subtitle;
    showFlashScreen(title, controllerOtaProgressSubtitle(now), durationMs, kColorAccent2);
}

bool requestControllerOtaCheck(String* message) {
    if(controllerOtaActionBusy()) {
        if(message != nullptr) {
            *message = "OTA already running";
        }
        return false;
    }

    const bool wifiSent = sendControllerWifiConfigIfAvailable();
    const bool otaSent = sendAction("ota_check", "latest");
    if(message != nullptr) {
        if(otaSent && wifiSent) {
            *message = "Wi-Fi + OTA check sent";
        } else if(otaSent) {
            *message = "OTA check sent";
        } else {
            *message = "OTA send failed";
        }
    }
    if(otaSent) {
        beginControllerOtaBusy("CONTROLLER OTA", "Checking for update", kControllerOtaCheckBusyMs);
    }
    return otaSent;
}

bool requestControllerOtaDownload(String* message) {
    if(controllerOtaActionBusy()) {
        if(message != nullptr) {
            *message = "OTA already running";
        }
        return false;
    }

    const bool wifiSent = sendControllerWifiConfigIfAvailable();
    const bool otaSent = sendAction("ota_download_if_available", "latest");
    if(message != nullptr) {
        if(otaSent && wifiSent) {
            *message = "Wi-Fi + OTA download sent";
        } else if(otaSent) {
            *message = "OTA download sent";
        } else {
            *message = "OTA send failed";
        }
    }
    if(otaSent) {
        clearControllerOtaDownloadArm();
        beginControllerOtaBusy("CONTROLLER OTA", "Downloading update", kControllerOtaDownloadBusyMs);
    }
    return otaSent;
}

bool requestControllerOta(String* message) {
    if(controllerOtaDownloadArmed()) {
        return requestControllerOtaDownload(message);
    }
    return requestControllerOtaCheck(message);
}

void confirmControllerOtaReady() {
    if(!controllerOtaReady()) {
        setToast("No controller OTA ready", 1200);
        return;
    }

    if(!dangerIsArmed(Screen::About, 0)) {
        armDanger(Screen::About, 0);
        setToast("Press again to update controller", remote_config::kDangerConfirmMs);
        app.redraw = true;
        return;
    }

    app.dangerArmUntil = 0;
    const bool sent = sendAction("ota_apply_ready_reboot");
    if(sent) {
        beginControllerOtaBusy("CONTROLLER OTA", "Applying update", kControllerOtaApplyBusyMs);
    }
    app.lastStatusPollAt = 0;
    setToast(sent ? "Controller OTA applying" : "OTA apply send failed", 1800);
    app.redraw = true;
}

void confirmRemoteOtaUpdate() {
    if(!remoteOtaUpdateAvailable()) {
        setToast(app.otaUpdate.checked ? "Remote is current" : "No remote OTA ready", 1400);
        return;
    }

    if(!dangerIsArmed(Screen::About, 1)) {
        armDanger(Screen::About, 1);
        setToast("Press again to update remote", remote_config::kDangerConfirmMs);
        app.redraw = true;
        return;
    }

    app.dangerArmUntil = 0;
    showFlashScreen("REMOTE OTA", "Downloading update", 120000, kColorGood);
    g_remoteOtaRequested = true;
}

void serviceRemoteOtaUpdate() {
    if(!g_remoteOtaRequested) {
        return;
    }
    g_remoteOtaRequested = false;
    Serial.println("[remote_ota] GUI download started");
    String message;
    const bool ok = downloadOtaManifest("latest", &message);
    Serial.printf("[remote_ota] download ok=%d %s\n", ok, message.c_str());
    if(!ok) {
        app.flashUntil = 0;
        setToast(String("OTA ERR: ") + message, 2800);
        app.redraw = true;
        return;
    }

    String bootMessage;
    const bool bootOk = setOtaBootPartition("next", &bootMessage);
    if(!bootOk) {
        app.flashUntil = 0;
        setToast(String("OTA BOOT ERR: ") + bootMessage, 2800);
        app.redraw = true;
        return;
    }

    Serial.printf("[remote_ota] %s; rebooting\n", bootMessage.c_str());
    delay(150);
    Serial.flush();
    ESP.restart();
}

String systemOtaToken(String& text) {
    text.trim();
    const int split = text.indexOf(' ');
    if(split < 0) { String token = text; text = ""; return token; }
    String token = text.substring(0, split);
    text.remove(0, split + 1);
    return token;
}

bool systemOtaShaMatches(const String& a, const String& b) {
    return a.length() >= 7 && b.length() >= 7 && (a.startsWith(b) || b.startsWith(a));
}

void finishSystemOta(bool ok, const String& detail) {
    if(ok) g_systemOta.flow.enter(sg98_system_ota::Stage::Complete, millis());
    else g_systemOta.flow.fail(millis());
    g_systemOta.detail = detail;
    g_systemOta.remoteInstallRequested = false;
    if(g_statePrefsReady) g_statePrefs.remove("sysPending");
    if(g_networkPending) {
        // The TLS worker owns the active Wi-Fi socket. Restore the user's
        // transport only after that worker reports its result.
        g_systemOta.restoreTransportPending = true;
    } else if(g_systemOta.previousTransport != LinkTransport::None) {
        switchTransportNow(g_systemOta.previousTransport);
        app.transportOverrideActive = g_systemOta.previousOverride;
    }
    app.flashUntil = 0;
    app.screen = Screen::About;
    setToast(detail, 6000);
    app.redraw = true;
    Serial.printf("[system_ota] %s: %s\n", ok ? "complete" : "stopped", detail.c_str());
}

void confirmSystemOta() {
    if(systemOtaBusy()) return;
    if(!isLinkOnline() || !app.hasControllerStatus) {
        setToast("Connect grinder first", 2000);
        return;
    }
    if(isRunningState() || app.pendingAction.kind != PendingActionKind::None ||
       app.pendingTarget.active || app.targetDirty || g_networkPending || g_autoOtaWifiStartedAt != 0) {
        setToast("Wait until idle", 2000);
        return;
    }
    if(!remote_wifi_has_saved_network()) {
        setToast("Save home Wi-Fi first", 2400);
        return;
    }
    app.screen = Screen::About;
    app.editing = false;
    if(!dangerIsArmed(Screen::About, 2)) {
        // A legacy controller OTA check can leave the old per-device UI busy
        // while its staged image is ready. The coordinated flow owns OTA state.
        clearControllerOtaBusy();
        clearControllerOtaDownloadArm();
        armDanger(Screen::About, 2);
        setToast("Press again: update both", remote_config::kDangerConfirmMs);
        app.redraw = true;
        return;
    }
    app.dangerArmUntil = 0;
    clearControllerOtaBusy();
    clearControllerOtaDownloadArm();
    g_systemOta.previousTransport = app.currentTransport;
    g_systemOta.previousOverride = app.transportOverrideActive;
    g_systemOta.expectedVersion = "";
    g_systemOta.expectedSha = "";
    g_systemOta.sentStage = sg98_system_ota::Stage::Idle;
    g_systemOta.detail = "Download both, update locally";
    g_systemOta.lastProbeAt = 0;
    g_systemOta.lastProgressAt = 0;
    g_systemOta.restoreTransportPending = false;
    g_systemOta.remoteBootVerificationPending = false;
    g_systemOta.downloadPart = 0;
    g_systemOta.downloadWritten = 0;
    g_systemOta.downloadTotal = 0;
    g_systemOta.controllerImageOffset = 0;
    g_systemOta.transferSession = 0;
    g_systemOta.transferOffset = 0;
    g_systemOta.transferPhase = 0;
    g_systemOta.transferAwaitingAck = false;
    g_systemOta.transferSendError = "";
    g_systemOta.controllerLocalPush = false;
    g_systemOta.legacyUpdateSent = false;
    g_systemOta.flow.start(millis());
    app.transportOverrideActive = true;
    app.dockClockActive = false;
    app.redraw = true;
}

bool handleSystemOtaAck(const sg98_protocol::AckFrame& ack) {
    using namespace sg98_system_ota;
    String text = ack.message;
    const uint32_t now = millis();
    if(ack.ok && text.startsWith("ota state ")) {
        g_systemOta.controllerIdentityProbePending = false;
        g_systemOta.controllerIdentityLegacy = false;
        text.remove(0, 10);
        const String version = systemOtaToken(text);
        const String sha = systemOtaToken(text);
        const String env = systemOtaToken(text);
        const String ui = systemOtaToken(text);
        const String local = systemOtaToken(text);
        const String idle = systemOtaToken(text);
        const String capabilities = systemOtaToken(text);
        if((ui != "full" && ui != "headless") || (local != "local" && local != "remote") ||
           (idle != "idle" && idle != "busy") || version.isEmpty() || sha.length() < 7) return true;
        g_systemOta.controllerVersion = version;
        g_systemOta.controllerSha = sha;
        g_systemOta.controllerEnv = env;
        g_systemOta.controllerUi = ui == "full" ? "Full controller" : "Internal controller";
        g_systemOta.controllerLocalUi = local == "local";
        g_systemOta.controllerLocalPush = capabilities.indexOf("local-push") >= 0;
        if(systemOtaBusy()) {
            if(idle != "idle") {
                finishSystemOta(false, "Controller not idle; remote unchanged");
            } else if(g_systemOta.flow.stage == Stage::Connect) {
                g_systemOta.flow.receive(Event::ControllerReady, now);
                g_systemOta.detail = "Controller ready; joining Wi-Fi";
            } else if(g_systemOta.flow.stage == Stage::LocalConnect) {
                if(version == g_systemOta.expectedVersion &&
                   systemOtaShaMatches(sha, g_systemOta.expectedSha)) {
                    g_systemOta.flow.enter(Stage::ControllerVerify, now);
                    g_systemOta.flow.receive(Event::ControllerVerified, now);
                    g_systemOta.detail = "Controller already current";
                } else if(g_systemOta.controllerLocalPush) {
                    g_systemOta.flow.receive(Event::LocalConnected, now);
                    g_systemOta.detail = "Controller connected; sending firmware";
                } else {
                    g_systemOta.flow.receive(Event::LegacyUpdateRequired, now);
                    g_systemOta.detail = "One-time controller compatibility update";
                }
            } else if(g_systemOta.flow.stage == Stage::LegacyControllerUpdate &&
                      version == g_systemOta.expectedVersion &&
                      systemOtaShaMatches(sha, g_systemOta.expectedSha) &&
                      g_systemOta.controllerLocalPush) {
                g_systemOta.flow.enter(Stage::ControllerVerify, now);
                g_systemOta.flow.receive(Event::ControllerVerified, now);
                g_systemOta.detail = "Controller compatibility update verified";
            } else if(g_systemOta.flow.stage == Stage::ControllerVerify &&
                      version == g_systemOta.expectedVersion &&
                      systemOtaShaMatches(sha, g_systemOta.expectedSha)) {
                if(g_systemOta.remoteBootVerificationPending) {
                    g_systemOta.remoteBootVerificationPending = false;
                    finishSystemOta(true, "Both devices verified current");
                } else {
                    g_systemOta.flow.receive(Event::ControllerVerified, now);
                    g_systemOta.detail = "Controller verified";
                }
            }
        }
        app.redraw = true;
        return true;
    }
    if(!systemOtaBusy()) {
        if(g_systemOta.controllerIdentityProbePending && !ack.ok && text == "unknown action") {
            g_systemOta.controllerIdentityProbePending = false;
            g_systemOta.controllerIdentityLegacy = true;
            g_systemOta.controllerUi = "Unknown/legacy";
            app.redraw = true;
            return true;
        }
        return false;
    }
    if(g_systemOta.flow.stage == Stage::LegacyControllerUpdate) {
        if(!ack.ok) {
            finishSystemOta(false, String("Legacy controller: ") + text);
        } else if(text == "wifi saved") {
            g_systemOta.detail = "Controller Wi-Fi configured";
        } else if(text.indexOf("reboot queued") >= 0) {
            g_systemOta.detail = "Legacy controller rebooting";
            g_systemOta.lastProbeAt = now;
        }
        app.redraw = true;
        return true;
    }
    return true;
}

bool systemOtaParseSha(const String& value, uint8_t out[sg98_ota_transport::kSha256Bytes]) {
    if(value.length() != sg98_ota_transport::kSha256Bytes * 2) return false;
    for(size_t index = 0; index < sg98_ota_transport::kSha256Bytes; ++index) {
        const int high = sg98_protocol::detail::hexValue(value[index * 2]);
        const int low = sg98_protocol::detail::hexValue(value[index * 2 + 1]);
        if(high < 0 || low < 0) return false;
        out[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool takeSystemOtaAck(sg98_ota_transport::AckFrame* ack) {
    if(ack == nullptr) return false;
    bool available = false;
    portENTER_CRITICAL(&g_systemOtaAckMux);
    if(g_systemOtaAckPending) {
        *ack = g_systemOtaAck;
        g_systemOtaAckPending = false;
        available = true;
    }
    portEXIT_CRITICAL(&g_systemOtaAckMux);
    return available;
}

bool sendSystemOtaTransferFrame() {
    using namespace sg98_ota_transport;
    const uint8_t* peer = activeEspNowPeerMac();
    if(peer == nullptr) {
        g_systemOta.transferSendError = "controller peer lost";
        return false;
    }
    const uint8_t phase = g_systemOta.transferPhase;
    if(phase == 0) {
        BeginFrame frame;
        frame.header.type = Type::Begin;
        frame.header.session = g_systemOta.transferSession;
        frame.imageSize = g_systemOta.controllerManifest.size;
        if(!systemOtaParseSha(g_systemOta.controllerManifest.sha256, frame.sha256)) {
            g_systemOta.transferSendError = "controller checksum invalid";
            return false;
        }
        strncpy(frame.release, g_systemOta.controllerManifest.version.c_str(), sizeof(frame.release) - 1);
        if(sendEspNowBytes(peer, reinterpret_cast<const uint8_t*>(&frame), sizeof(frame))) return true;
        g_systemOta.transferSendError = "ESP-NOW begin send failed";
        return false;
    }
    if(phase == 1) {
        DataFrame frame;
        frame.header.type = Type::Data;
        frame.header.session = g_systemOta.transferSession;
        frame.offset = g_systemOta.transferOffset;
        frame.length = static_cast<uint16_t>(min<uint32_t>(
            sizeof(frame.data), g_systemOta.controllerManifest.size - frame.offset));
        const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
        if(partition == nullptr ||
           esp_partition_read(partition, g_systemOta.controllerImageOffset + frame.offset,
                              frame.data, frame.length) != ESP_OK) {
            g_systemOta.transferSendError = "staged controller image unreadable";
            return false;
        }
        frame.crc32 = crc32(frame.data, frame.length);
        const size_t length = offsetof(DataFrame, data) + frame.length;
        if(sendEspNowBytes(peer, reinterpret_cast<const uint8_t*>(&frame), length)) return true;
        g_systemOta.transferSendError = "ESP-NOW data send failed";
        return false;
    }
    ControlFrame frame;
    frame.header.type = phase == 2 ? Type::End : Type::Commit;
    frame.header.session = g_systemOta.transferSession;
    if(sendEspNowBytes(peer, reinterpret_cast<const uint8_t*>(&frame), sizeof(frame))) return true;
    g_systemOta.transferSendError = "ESP-NOW control send failed";
    return false;
}

void serviceSystemOtaTransfer(uint32_t now) {
    using namespace sg98_ota_transport;
    AckFrame ack;
    if(takeSystemOtaAck(&ack) && ack.header.session == g_systemOta.transferSession) {
        app.lastLinkActivityAt = now;
        g_systemOta.transferAwaitingAck = false;
        g_systemOta.transferRetries = 0;
        if(ack.status == Status::Error || ack.status == Status::Busy) {
            finishSystemOta(false, String("Controller: ") + ack.message);
            return;
        }
        if(g_systemOta.transferPhase == 0 && ack.status == Status::Accepted) {
            g_systemOta.transferOffset = ack.nextOffset;
            g_systemOta.transferPhase = 1;
        } else if(g_systemOta.transferPhase == 1 && ack.status == Status::Accepted) {
            if(ack.nextOffset > g_systemOta.transferOffset) {
                g_systemOta.flow.enteredAt = now;
            }
            g_systemOta.transferOffset = ack.nextOffset;
            if(g_systemOta.transferOffset >= g_systemOta.controllerManifest.size) {
                g_systemOta.transferPhase = 2;
            }
        } else if(g_systemOta.transferPhase == 2 && ack.status == Status::Ready) {
            g_systemOta.transferPhase = 3;
        } else if(g_systemOta.transferPhase == 3 && ack.status == Status::Committed) {
            g_systemOta.flow.receive(sg98_system_ota::Event::TransferReady, now);
            g_systemOta.detail = "Controller rebooting";
            g_systemOta.lastProbeAt = now;
            g_systemOta.sentStage = sg98_system_ota::Stage::Idle;
            return;
        }
        app.redraw = true;
    }
    if(g_systemOta.transferAwaitingAck && now - g_systemOta.transferSentAt < 700) return;
    if(g_systemOta.transferAwaitingAck && ++g_systemOta.transferRetries > 15) {
        finishSystemOta(false, "Controller transfer timed out");
        return;
    }
    if(sendSystemOtaTransferFrame()) {
        g_systemOta.transferAwaitingAck = true;
        g_systemOta.transferSentAt = now;
    } else if(++g_systemOta.transferRetries > 15) {
        finishSystemOta(false, String("Controller transfer: ") + g_systemOta.transferSendError);
    }
}

void serviceSystemOta() {
    using namespace sg98_system_ota;
    const uint32_t now = millis();
    if(!systemOtaBusy()) {
        if(app.screen == Screen::About && isLinkOnline() &&
           !g_systemOta.controllerIdentityLegacy &&
           now - g_systemOta.lastProbeAt >= 5000) {
            g_systemOta.lastProbeAt = now;
            g_systemOta.controllerIdentityProbePending = sendAction("ota_status");
        }
        return;
    }
    app.lastUserActivityAt = now;
    app.lastControlActivityAt = now;
    if(g_systemOta.flow.expired(now)) {
        finishSystemOta(false, String(label(g_systemOta.flow.stage)) + " timed out");
        return;
    }
    const Stage stage = g_systemOta.flow.stage;
    if(g_systemOta.lastProgressAt == 0 || now - g_systemOta.lastProgressAt >= 1000 || g_systemOta.sentStage != stage) {
        showFlashScreen("SYSTEM UPDATE", label(stage), 250000, kColorAccent2);
        g_systemOta.lastProgressAt = now;
    }
    if(stage == Stage::Connect) {
        if(isLinkOnline() && (g_systemOta.sentStage != stage || now - g_systemOta.lastProbeAt >= 2000)) {
            g_systemOta.controllerIdentityProbePending = sendAction("ota_status");
            g_systemOta.sentStage = stage;
            g_systemOta.lastProbeAt = now;
        }
    } else if(stage == Stage::WifiConnect) {
        if(g_systemOta.sentStage != stage) {
            switchTransportNow(LinkTransport::WebSocket);
            stopWebSocketSession();
            app.transportOverrideActive = true;
            g_systemOta.sentStage = stage;
        }
        if(remote_wifi_sta_connected()) g_systemOta.flow.receive(Event::WifiConnected, now);
    } else if(stage == Stage::DownloadFiles) {
        if(g_systemOta.sentStage != stage && !g_networkPending &&
           queueBackgroundNetwork(BackgroundNetworkKind::SystemOtaStage, "")) {
            g_systemOta.sentStage = stage;
        }
    } else if(stage == Stage::LocalConnect) {
        if(isLinkOnline() && (g_systemOta.sentStage != stage || now - g_systemOta.lastProbeAt >= 2000)) {
            sendAction("ota_status");
            g_systemOta.sentStage = stage;
            g_systemOta.lastProbeAt = now;
        }
    } else if(stage == Stage::LegacyControllerUpdate) {
        if(!g_systemOta.legacyUpdateSent) {
            const bool wifiSent = sendControllerWifiConfigIfAvailable();
            const bool otaSent = sendAction("ota_manifest_reboot", "latest");
            if(!otaSent) {
                finishSystemOta(false, "Could not start legacy controller update");
                return;
            }
            g_systemOta.legacyUpdateSent = true;
            g_systemOta.sentStage = stage;
            g_systemOta.lastProbeAt = now;
            g_systemOta.detail = wifiSent ? "Legacy controller downloading over Wi-Fi"
                                          : "Legacy controller update requested";
        } else if(isLinkOnline() && now - g_systemOta.lastProbeAt >= 2000) {
            sendAction("ota_status");
            g_systemOta.lastProbeAt = now;
        }
    } else if(stage == Stage::ControllerTransfer) {
        if(g_systemOta.transferSession == 0) {
            g_systemOta.transferSession = static_cast<uint16_t>((esp_random() & 0xFFFFU) | 1U);
            g_systemOta.transferOffset = 0;
            g_systemOta.transferPhase = 0;
            g_systemOta.transferRetries = 0;
            g_systemOta.transferAwaitingAck = false;
        }
        serviceSystemOtaTransfer(now);
    } else if(stage == Stage::ControllerVerify) {
        if(g_systemOta.sentStage != stage || now - g_systemOta.lastProbeAt >= 2000) {
            if(isLinkOnline()) sendAction("ota_status");
            g_systemOta.sentStage = stage;
            g_systemOta.lastProbeAt = now;
        }
    } else if(stage == Stage::RemoteInstall) {
        g_systemOta.remoteInstallRequested = true;
    }
}

void serviceSystemRemoteInstall() {
    if(!g_systemOta.remoteInstallRequested || !g_systemOta.flow.controllerVerified) return;
    g_systemOta.remoteInstallRequested = false;
    const auto& spec = g_systemOta.remoteManifest;
    // Both images were already downloaded and hash-verified before Wi-Fi was released.
    if(!g_statePrefsReady || g_statePrefs.putString("sysVersion", spec.version) == 0 ||
       g_statePrefs.putString("sysSha", spec.gitSha) == 0 ||
       g_statePrefs.putUChar("sysTransport", static_cast<uint8_t>(g_systemOta.previousTransport)) == 0 ||
       g_statePrefs.putBool("sysOverride", g_systemOta.previousOverride) == 0 ||
       g_statePrefs.putBool("sysPending", true) == 0) {
        finishSystemOta(false, "Could not save remote verification state");
        return;
    }
    String message;
    if(!setOtaBootPartition("next", &message)) { finishSystemOta(false, message); return; }
    Serial.printf("[system_ota] staged remote selected; %s; rebooting\n", message.c_str());
    delay(150);
    Serial.flush();
    ESP.restart();
}

String actionFeedbackLabel(const SettingItem& item) {
    if(item.action == nullptr) {
        if(isGaugeSetItem(item)) {
            return "Gap set";
        }
        return String(item.label) + " applied";
    }
    if(strcmp(item.action, "reset_network") == 0) return "Network reset";
    if(strcmp(item.action, "reset_defaults") == 0) return "Defaults reset";
    if(strcmp(item.action, "about") == 0) return sg98_version::aboutVersionLine();
    if(strcmp(item.action, "voice") == 0) return "Voice command";
    if(strcmp(item.action, "calibrate") == 0) return "Calibration started";
    if(strcmp(item.action, "controller_ota") == 0) {
        return controllerOtaDownloadArmed() ? "OTA download requested" : "OTA check requested";
    }
    if(strcmp(item.action, "redetect") == 0) return "Detecting drive";
    if(strcmp(item.action, "drive") == 0) return "Drive applied";
    if(strcmp(item.action, "jmc_baud") == 0) return "Baud applied";
    return String(item.label) + " applied";
}

void runConfirmAction(const SettingItem& item) {
    if(item.action == nullptr) {
        return;
    }
    if(strcmp(item.action, "system_ota") == 0) { confirmSystemOta(); return; }

    if(strcmp(item.action, "about") == 0) {
        setToast(sg98_version::buildId(), 2600);
        return;
    }

    if(strcmp(item.action, "reset_network") == 0) {
        remote_wifi_reset_network_settings();
        clearPollSchedule();
        switchTransportNow(initialTransport());
        requestFullControllerRefresh();
        markPersistentStateDirty();
        setToast(actionFeedbackLabel(item), 1200);
        return;
    }

    if(strcmp(item.action, "controller_ota") == 0) {
        String message;
        if(requestControllerOta(&message)) {
            app.lastStatusPollAt = 0;
            setToast(message, 1400);
        } else {
            setToast(message, 1200);
        }
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
        PendingActionKind pendingKind = PendingActionKind::None;
        const char* expectedLabel = nullptr;
        if(forcedDriveForArg(item.actionArg, nullptr, &pendingKind, &expectedLabel)) {
            queued = queuePendingAction(pendingKind,
                                        PendingActionReflection::Drive,
                                        item.actionArg,
                                        expectedLabel,
                                        0);
        }
    } else if(strcmp(item.action, "jmc_baud") == 0 && item.actionArg != nullptr) {
        if(!activeDriveIsJmc()) {
            setToast("JMC only", 1000);
            return;
        }
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
        setToast(actionFeedbackLabel(item), 1200);
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
            if(isGaugeSetItem(item) && !gaugeSetMenuValueKnown()) {
                requestMenu(1);
                setToast("Syncing gauge set", 900);
                app.redraw = true;
                return;
            }
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
                const bool allowGaugeSetWrite = isGaugeSetItem(item) && app.screen == Screen::Gbw;
                if(applyMenuValue(item, nextValue, allowGaugeSetWrite)) {
                    if(isRadioModeItem(item)) {
                        if(safeUserRadioMode(nextValue) == kControllerRadioOff) {
                            setToast("Radio off", 1200);
                        } else {
                            setToast(transportFromRadioMode(nextValue) == LinkTransport::WebSocket ? "WiFi mode" : "ESP-NOW mode", 1200);
                        }
                    } else {
                        setToast(actionFeedbackLabel(item), 1200);
                    }
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
            setToast(actionFeedbackLabel(item), 1200);
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
        } else if(item.actionArg != nullptr && strcmp(item.actionArg, "about") == 0) {
            setScreen(Screen::About);
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

    if(item.action != nullptr && strcmp(item.action, "voice") == 0) {
        String message;
        (void)runVoiceCommand(&message, true);
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
        LocalDriveSelection selection = LOCAL_DRIVE_AUTO;
        if(forcedDriveForArg(item.actionArg, &selection, nullptr, nullptr)) {
            setLocalDriveSelection(selection);
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

    exitDockClock();
    const Screen previousScreen = app.screen;
    app.screen = screen;
    app.menuRollOffset = 0.0f;
    app.menuEncoderRemainder = 0;
    app.editing = false;
    app.redraw = true;
    requestVisibleMenuIfNeeded(true);
    sanitizeSelections();
    if(previousScreen != Screen::Profiler && screen == Screen::Profiler) {
        armPressureProfiler();
    } else if(previousScreen == Screen::Profiler && screen != Screen::Profiler) {
        leavePressureProfiler();
    }
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
        app.draftValue = isRadioModeItem(item)
                             ? stepRadioModeValue(app.draftValue, delta)
                             : clampValue(app.draftValue + delta * item.step, item.minValue, item.maxValue);
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
    if(app.screen == Screen::About && !app.editing) {
        confirmSystemOta();
        return;
    }

    if(app.screen == Screen::Profiler && !app.editing) {
        armPressureProfiler();
        return;
    }

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

void deinitEspNowSessionOnly() {
    if(app.espNowReady) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        app.espNowReady = false;
    }
}

void stopEspNowSession() {
    deinitEspNowSessionOnly();
    if(!remote_wifi_is_active()) {
        esp_wifi_stop();
        WiFi.mode(WIFI_OFF);
    }
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
    if(systemOtaBusy()) return false;
    if(remote_config::kDisableIdleSleepForTesting) {
        return false;
    }
    if(remote_config::kUseDockClockForIdle) {
        return false;
    }
    if(kDisableTransportPerfTest) {
        return false;
    }
    if(app.battery.usbPresent) {
        return false;
    }
    if(app.editing || app.targetDirty || isRunningState() ||
       (app.screen != Screen::Profiler && remote_pressure_ble_connected())) {
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
    if(remote_board_wake_supported(RemoteWakeSource::SecondaryButton)) {
        wakeConfigured = configureWakePin(pins.secondaryButton) || wakeConfigured;
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
    clearWakePin(pins.secondaryButton);
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
    if(pins.secondaryButton >= 0 &&
       (wakeMask & (uint64_t{1} << static_cast<unsigned>(pins.secondaryButton))) != 0) {
        pinMode(pins.secondaryButton, INPUT_PULLUP);
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
    const uint64_t wakeMask =
        app.screen != Screen::Profiler && idleDeepSleepSupported() && !touchWakeNeedsLightSleep()
            ? deepSleepWakeMask()
            : 0;
    if(wakeMask != 0 && enterIdleDeepSleep(wakeMask)) {
        return;
    }

    enterIdleLightSleep(resumeTransport);
}

bool primaryLongPressShouldBackInMenu() {
    return useRoundNavigation() &&
           remote_board_info().id == RemoteBoardId::LilyGoT4S3H678 &&
           (app.editing || app.screen != Screen::Home);
}

void handleRoundBackAction() {
    if(app.editing) {
        cancelEditing();
    } else if(app.screen == Screen::Profiler) {
        setScreen(Screen::Home);
    } else if(app.screen == Screen::Home) {
        if(isRunningState()) {
            return;
        }
        setScreen(Screen::Main);
    } else if(app.screen == Screen::Drive) {
        setScreen(Screen::Main);
    } else if(app.screen == Screen::EspNowPeers) {
        setScreen(Screen::Network);
    } else if(app.screen == Screen::About) {
        setScreen(Screen::Main);
    } else if(app.screen != Screen::Home) {
        setScreen(Screen::Home);
    }
}

void handlePrimaryLongPress() {
    if(primaryLongPressShouldBackInMenu()) {
        handleRoundBackAction();
        return;
    }

    if(useRoundNavigation() && app.screen == Screen::Home && !app.editing) {
        if(isRunningState()) {
            setToast("Stop first", 900);
            return;
        }
        setScreen(Screen::Profiler);
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
        if(isRunningState()) {
            setToast("Stop first", 900);
            return;
        }
        setScreen(Screen::Profiler);
    }
}

void handleSecondaryShortPress() {
    if(useRoundNavigation()) {
        handleRoundBackAction();
        return;
    }
    if(app.screen == Screen::About) {
        setScreen(Screen::Main);
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

    if(app.screen == Screen::Profiler) {
        touchTracker.mode = TouchMode::Tap;
        return;
    }

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
    const int16_t dy = static_cast<int16_t>(y - touchTracker.lastY);
    touchTracker.scrollAccum += menuTouchScrollDelta(dy);
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
    if(isDuplicateTouchTap(x, y)) {
        return;
    }
    rememberTouchTap(x, y);

    if(app.screen == Screen::About && !app.editing) {
        const Layout layout = buildLayout();
        if(useRoundNavigation() && handleRoundNavTouch(x, y)) return;
        confirmSystemOta();
        return;
    }

    if(useRoundNavigation()) {
        if(app.screen != Screen::Home && !app.editing) {
            if(handleRoundNavTouch(x, y)) {
                return;
            }
            handleMenuTouch(x, y);
            return;
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

void handleTouchLongPress(int16_t x, int16_t y) {
    debugRoundInput("touch_long", x, y, static_cast<int32_t>(app.screen), app.editing ? 1 : 0);
    Serial.printf("[remote][input] touch long x=%d y=%d screen=%s editing=%d\n",
                  static_cast<int>(x),
                  static_cast<int>(y),
                  screenTitle(app.screen),
                  app.editing ? 1 : 0);

    if(app.editing) {
        cancelEditing();
        return;
    }

    if(app.screen == Screen::Home) {
        if(isRunningState()) {
            setToast("Stop first", 900);
            return;
        }
        setScreen(Screen::Main);
        return;
    }

    if(app.screen == Screen::Profiler) {
        setScreen(Screen::Home);
        return;
    }

    handleSecondaryShortPress();
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
        if(!touchTracker.dragUsed && heldMs >= kTouchLongPressMs) {
            handleTouchLongPress(touchTracker.startX, touchTracker.startY);
            touchTracker = {};
            return;
        }
        if(useRoundNavigation() && !touchTracker.dragUsed &&
           heldMs <= remote_config::kTapMaxDurationMs) {
            handleTouchTap(touchTracker.startX, touchTracker.startY);
            touchTracker = {};
            return;
        }
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
        case 5: return NUM_MENU5_ITEMS;
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
                return settingItemVisible(item) ? &item : nullptr;
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
    out += ",\"envSyncFieldsPresent\":";
    out += app.status.envSyncFieldsPresent ? "true" : "false";
    out += ",\"envTimeValid\":";
    out += app.status.envTimeValid ? "true" : "false";
    out += ",\"envWeatherValid\":";
    out += app.status.envWeatherValid ? "true" : "false";
    out += ",\"burrGapFieldsPresent\":";
    out += app.status.burrGapFieldsPresent ? "true" : "false";
    out += ",\"burrGapEnabled\":";
    out += app.status.burrGapEnabled ? "true" : "false";
    out += ",\"burrGapKnown\":";
    out += app.status.burrGapKnown ? "true" : "false";
    out += ",\"burrGapLive\":";
    out += app.status.burrGapLive ? "true" : "false";
    out += ",\"burrGapUm\":";
    out += String(app.status.burrGapUm);
    out += ",\"controllerOtaReadyFieldsPresent\":";
    out += app.status.controllerOtaReadyFieldsPresent ? "true" : "false";
    out += ",\"controllerOtaReady\":";
    out += app.status.controllerOtaReady ? "true" : "false";
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

void appendBatteryJson(String& out, const RemoteBatteryStatus& battery) {
    out += '{';
    out += "\"supported\":";
    out += battery.supported ? "true" : "false";
    out += ",\"batteryPresent\":";
    out += battery.batteryPresent ? "true" : "false";
    out += ",\"usbPresent\":";
    out += battery.usbPresent ? "true" : "false";
    out += ",\"charging\":";
    out += battery.charging ? "true" : "false";
    out += ",\"discharging\":";
    out += battery.discharging ? "true" : "false";
    out += ",\"standby\":";
    out += battery.standby ? "true" : "false";
    out += ",\"percent\":";
    out += String(static_cast<int>(battery.percent));
    out += ",\"millivolts\":";
    out += String(battery.millivolts);
    out += ",\"vbusMillivolts\":";
    out += String(battery.vbusMillivolts);
    out += ",\"systemMillivolts\":";
    out += String(battery.systemMillivolts);
    out += '}';
}

void setInputDiagnosticEvent(const char* eventName, uint32_t now) {
    g_inputDiagnostics.lastEventAt = now;
    if(eventName == nullptr) {
        eventName = "unknown";
    }
    strncpy(g_inputDiagnostics.lastEvent, eventName, sizeof(g_inputDiagnostics.lastEvent) - 1);
    g_inputDiagnostics.lastEvent[sizeof(g_inputDiagnostics.lastEvent) - 1] = '\0';
}

void updateInputDiagnostics(const RemoteInputState& input, uint32_t now) {
    g_inputDiagnostics.primaryPressed = input.primaryPressed;
    g_inputDiagnostics.secondaryPressed = input.secondaryPressed;
    g_inputDiagnostics.touchPressed = input.touch.pressed;
    if(input.touch.pressed || input.touch.justPressed || input.touch.justReleased) {
        g_inputDiagnostics.touchX = input.touch.x;
        g_inputDiagnostics.touchY = input.touch.y;
    }

    if(input.primaryLongPress) {
        ++g_inputDiagnostics.primaryLongCount;
        setInputDiagnosticEvent("primary_long", now);
    } else if(input.primaryShortPress) {
        ++g_inputDiagnostics.primaryShortCount;
        setInputDiagnosticEvent("primary_short", now);
    }

    if(input.secondaryLongPress) {
        ++g_inputDiagnostics.secondaryLongCount;
        setInputDiagnosticEvent("secondary_long", now);
    } else if(input.secondaryShortPress) {
        ++g_inputDiagnostics.secondaryShortCount;
        setInputDiagnosticEvent("secondary_short", now);
    }

    if(input.touch.justPressed) {
        ++g_inputDiagnostics.touchPressCount;
        setInputDiagnosticEvent("touch_press", now);
    } else if(input.touch.justReleased) {
        ++g_inputDiagnostics.touchReleaseCount;
        setInputDiagnosticEvent("touch_release", now);
    }

    if(input.motionDetected) {
        ++g_inputDiagnostics.motionCount;
        setInputDiagnosticEvent("motion", now);
    }
    if(input.displayRotationChanged) {
        ++g_inputDiagnostics.displayRotationCount;
        g_inputDiagnostics.displayRotation = input.displayRotation;
        setInputDiagnosticEvent("display_rotation", now);
    }
    if(input.powerStatusChanged) {
        ++g_inputDiagnostics.powerChangeCount;
        setInputDiagnosticEvent("power_change", now);
    }
}

void appendInputDiagnosticsJson(String& out) {
    out += '{';
    out += "\"lastEvent\":";
    appendJsonQuoted(out, g_inputDiagnostics.lastEvent);
    out += ",\"lastEventAgeMs\":";
    out += g_inputDiagnostics.lastEventAt == 0
               ? String(-1)
               : String(static_cast<int32_t>(millis() - g_inputDiagnostics.lastEventAt));
    out += ",\"primaryPressed\":";
    out += g_inputDiagnostics.primaryPressed ? "true" : "false";
    out += ",\"primaryShortCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.primaryShortCount));
    out += ",\"primaryLongCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.primaryLongCount));
    out += ",\"secondaryPressed\":";
    out += g_inputDiagnostics.secondaryPressed ? "true" : "false";
    out += ",\"secondaryShortCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.secondaryShortCount));
    out += ",\"secondaryLongCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.secondaryLongCount));
    out += ",\"touchPressed\":";
    out += g_inputDiagnostics.touchPressed ? "true" : "false";
    out += ",\"touchPressCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.touchPressCount));
    out += ",\"touchReleaseCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.touchReleaseCount));
    out += ",\"touchX\":";
    out += String(g_inputDiagnostics.touchX);
    out += ",\"touchY\":";
    out += String(g_inputDiagnostics.touchY);
    out += ",\"motionCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.motionCount));
    out += ",\"displayRotationCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.displayRotationCount));
    out += ",\"displayRotation\":";
    out += String(static_cast<unsigned>(g_inputDiagnostics.displayRotation));
    out += ",\"powerChangeCount\":";
    out += String(static_cast<unsigned long>(g_inputDiagnostics.powerChangeCount));
    out += '}';
}

void appendResourceDiagnosticsJson(String& out) {
    out += '{';
    out += "\"heapFree\":";
    out += String(static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    out += ",\"heapMinFree\":";
    out += String(static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
    out += ",\"psramFree\":";
    out += String(static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    out += ",\"psramMinFree\":";
    out += String(static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
    out += '}';
}

int32_t ageMsOrMinusOne(uint32_t timestamp, uint32_t now) {
    if(timestamp == 0) {
        return -1;
    }
    return static_cast<int32_t>(now - timestamp);
}

void appendDockClockJson(String& out) {
    const uint32_t nowMs = millis();
    const time_t unixNow = time(nullptr);
    struct tm timeInfo = {};
    const bool timeValid = localClockTime(&timeInfo);

    out += '{';
    out += "\"active\":";
    out += app.dockClockActive ? "true" : "false";
    out += ",\"timeConfigured\":";
    out += app.dockClockTimeConfigured ? "true" : "false";
    out += ",\"timeValid\":";
    out += timeValid ? "true" : "false";
    out += ",\"unixTime\":";
    out += String(static_cast<long>(unixNow));
    out += ",\"lastNtpSyncAgeMs\":";
    out += String(ageMsOrMinusOne(app.dockClockLastNtpSyncAt, nowMs));
    out += ",\"weatherValid\":";
    out += app.dockWeather.valid ? "true" : "false";
    out += ",\"weatherLastFetchAgeMs\":";
    out += String(ageMsOrMinusOne(app.dockClockLastWeatherFetchAt, nowMs));
    out += ",\"weatherLastOkAgeMs\":";
    out += String(ageMsOrMinusOne(app.dockClockLastWeatherOkAt, nowMs));
    out += ",\"weatherHttpCode\":";
    out += String(app.dockClockLastWeatherHttpCode);
    out += ",\"location\":";
    appendJsonQuoted(out, dockLocationLine());
    out += ",\"temperatureF\":";
    out += String(static_cast<int>(app.dockWeather.temperatureF));
    out += ",\"weatherCode\":";
    out += String(static_cast<unsigned>(app.dockWeather.weatherCode));
    out += ",\"precipitationPercent\":";
    out += String(static_cast<int>(app.dockWeather.precipitationPercent));
    out += '}';
}

void appendRemoteStatusJson(String& out) {
    out += '{';
    out += "\"board\":";
    appendJsonQuoted(out, remote_board_info().name);
    out += ",\"firmwareVersion\":";
    appendJsonQuoted(out, sg98_version::release());
    out += ",\"firmwareGitSha\":";
    appendJsonQuoted(out, sg98_version::gitSha());
    out += ",\"firmwareDirty\":";
    out += sg98_version::dirty() ? "true" : "false";
    out += ",\"buildEnv\":";
    appendJsonQuoted(out, sg98_version::buildEnv());
    out += ",\"systemOta\":{\"stage\":";
    appendJsonQuoted(out, sg98_system_ota::label(g_systemOta.flow.stage));
    out += ",\"busy\":";
    out += systemOtaBusy() ? "true" : "false";
    out += ",\"controllerVerified\":";
    out += g_systemOta.flow.controllerVerified ? "true" : "false";
    out += ",\"controllerVersion\":";
    appendJsonQuoted(out, g_systemOta.controllerVersion);
    out += ",\"controllerGitSha\":";
    appendJsonQuoted(out, g_systemOta.controllerSha);
    out += ",\"controllerUi\":";
    appendJsonQuoted(out, g_systemOta.controllerUi);
    out += ",\"controllerHasLocalUi\":";
    out += g_systemOta.controllerLocalUi ? "true" : "false";
    out += ",\"detail\":";
    appendJsonQuoted(out, g_systemOta.detail);
    out += ",\"armed\":";
    out += dangerIsArmed(Screen::About, 2) ? "true" : "false";
    out += ",\"legacyControllerOtaBusy\":";
    out += controllerOtaActionBusy() ? "true" : "false";
    out += ",\"transferOffset\":";
    out += String(g_systemOta.transferOffset);
    out += ",\"transferTotal\":";
    out += String(g_systemOta.controllerManifest.size);
    out += ",\"transferRetries\":";
    out += String(static_cast<unsigned>(g_systemOta.transferRetries));
    out += ",\"transferPhase\":";
    out += String(static_cast<unsigned>(g_systemOta.transferPhase));
    out += '}';
    out += ",\"ota\":{\"checked\":";
    out += app.otaUpdate.checked ? "true" : "false";
    out += ",\"lastAttemptOk\":";
    out += app.otaUpdate.lastAttemptOk ? "true" : "false";
    out += ",\"updateAvailable\":";
    out += app.otaUpdate.updateAvailable ? "true" : "false";
    out += ",\"networkPending\":";
    out += g_networkPending ? "true" : "false";
    out += ",\"latestVersion\":";
    appendJsonQuoted(out, app.otaUpdate.latestVersion);
    out += ",\"message\":";
    appendJsonQuoted(out, app.otaUpdate.message);
    out += '}';
    out += ",\"screen\":";
    appendJsonQuoted(out, screenTitle(app.screen));
    out += ",\"controllerConnection\":";
    appendJsonQuoted(out, controllerConnectionText());
    out += ",\"networkInfo\":";
    appendJsonQuoted(out, networkCurveStatusText());
    out += ",\"remoteOtaArmed\":";
    out += dangerIsArmed(Screen::About, 1) ? "true" : "false";
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
    out += ",\"espNowChannel\":";
    out += String(static_cast<unsigned>(app.espNowChannel));
    uint8_t hardwareChannel = 0;
    wifi_second_chan_t secondaryChannel = WIFI_SECOND_CHAN_NONE;
    (void)esp_wifi_get_channel(&hardwareChannel, &secondaryChannel);
    out += ",\"espNowHardwareChannel\":";
    out += String(static_cast<unsigned>(hardwareChannel));
    out += ",\"espNowTxDelivered\":";
    out += String(static_cast<unsigned long>(g_espNowTxDelivered));
    out += ",\"espNowTxFailed\":";
    out += String(static_cast<unsigned long>(g_espNowTxFailed));
    out += ",\"espNowTxEnqueueFailed\":";
    out += String(static_cast<unsigned long>(g_espNowTxEnqueueFailed));
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
    out += ",\"battery\":";
    appendBatteryJson(out, app.battery);
    out += ",\"inputs\":";
    appendInputDiagnosticsJson(out);
    out += ",\"resources\":";
    appendResourceDiagnosticsJson(out);
    out += ",\"dockClock\":";
    appendDockClockJson(out);
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
    out += ",\"staConnecting\":";
    out += remote_wifi_sta_connecting() ? "true" : "false";
    out += ",\"staAttempts\":";
    out += String(static_cast<unsigned long>(remote_wifi_sta_attempt_count()));
    out += ",\"staPasswordLen\":";
    out += String(static_cast<unsigned>(remote_wifi_sta_password_length()));
    out += ",\"staDisconnectReason\":";
    out += String(static_cast<unsigned>(remote_wifi_sta_disconnect_reason()));
    out += ",\"staDisconnectReasonText\":";
    appendJsonQuoted(out, remote_wifi_sta_disconnect_reason_text());
    out += ",\"lastStaEvent\":";
    appendJsonQuoted(out, remote_wifi_sta_last_event());
    out += ",\"staSSID\":";
    appendJsonQuoted(out, remote_wifi_sta_ssid());
    out += ",\"staIP\":";
    appendJsonQuoted(out, remote_wifi_sta_ip());
    out += ",\"apSSID\":";
    appendJsonQuoted(out, remote_wifi_ap_ssid());
    out += ",\"apIP\":";
    appendJsonQuoted(out, remote_wifi_ap_ip());
    out += ",\"apRunning\":";
    out += remote_wifi_ap_running() ? "true" : "false";
    out += ",\"apStations\":";
    out += String(static_cast<unsigned>(remote_wifi_ap_station_count()));
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
    for(uint8_t menuNum = 1; menuNum <= kControllerMenuMax; ++menuNum) {
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

String stableRemoteMacAddress() {
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

String urlEncodeFormValue(const String& value) {
    static const char* kHex = "0123456789ABCDEF";
    String out;
    out.reserve(value.length() + 8);
    for(size_t i = 0; i < value.length(); ++i) {
        const uint8_t ch = static_cast<uint8_t>(value.charAt(i));
        if((ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' ||
           ch == '_' ||
           ch == '.' ||
           ch == '~') {
            out += static_cast<char>(ch);
        } else if(ch == ' ') {
            out += '+';
        } else {
            out += '%';
            out += kHex[(ch >> 4) & 0x0F];
            out += kHex[ch & 0x0F];
        }
    }
    return out;
}

const char* wifiStatusName(wl_status_t status) {
    switch(status) {
        case WL_CONNECTED: return "connected";
        case WL_NO_SSID_AVAIL: return "no_ssid";
        case WL_CONNECT_FAILED: return "connect_failed";
        case WL_CONNECTION_LOST: return "connection_lost";
        case WL_DISCONNECTED: return "disconnected";
        case WL_IDLE_STATUS: return "idle";
        default: return "unknown";
    }
}

struct WifiPhoneHttpResult {
    bool ok = false;
    int code = -1;
    size_t length = 0;
    String body;
    String error;
};

struct WifiPhoneApScanResult {
    bool found = false;
    int32_t channel = 0;
    int32_t rssi = -127;
    uint8_t bssid[6] = {};
};

void printWifiPhoneEvent(const char* step,
                         bool ok,
                         int code,
                         size_t length,
                         const String& detail,
                         const String& body = String(),
                         const String& needle = String()) {
    String out;
    out.reserve(512);
    out += "{\"type\":\"wifi_phone\",\"step\":";
    appendJsonQuoted(out, step);
    out += ",\"ok\":";
    out += ok ? "true" : "false";
    out += ",\"code\":";
    out += String(code);
    out += ",\"length\":";
    out += String(length);
    out += ",\"detail\":";
    appendJsonQuoted(out, detail);
    if(needle.length() > 0) {
        out += ",\"containsNeedle\":";
        out += body.indexOf(needle) >= 0 ? "true" : "false";
    }
    if(body.length() > 0) {
        String snippet = body.substring(0, min<size_t>(body.length(), 180));
        snippet.replace("\n", " ");
        snippet.replace("\r", " ");
        out += ",\"snippet\":";
        appendJsonQuoted(out, snippet);
    }
    out += '}';
    Serial.println(out);
}

WifiPhoneHttpResult wifiPhoneHttpRequest(const char* method, const String& path, const String& formBody = String()) {
    WifiPhoneHttpResult result;
    WiFiClient client;
    client.setTimeout(2500);
    const IPAddress controllerIp(192, 168, 4, 1);
    if(!client.connect(controllerIp, 80, 2500)) {
        result.error = "connect failed";
        return result;
    }
    client.setNoDelay(true);

    client.print(method);
    client.print(' ');
    client.print(path);
    client.print(" HTTP/1.0\r\nHost: 192.168.4.1\r\nConnection: close\r\n");
    if(formBody.length() > 0) {
        client.print("Content-Type: application/x-www-form-urlencoded;charset=UTF-8\r\nContent-Length: ");
        client.print(formBody.length());
        client.print("\r\n");
    }
    client.print("\r\n");
    if(formBody.length() > 0) {
        client.print(formBody);
    }

    const uint32_t startedAt = millis();
    uint32_t lastDataAt = startedAt;
    int expectedTotal = -1;
    while((millis() - startedAt) < 20000) {
        while(client.available() > 0) {
            const char ch = static_cast<char>(client.read());
            if(result.body.length() < 4096) {
                result.body += ch;
            }
            ++result.length;
            lastDataAt = millis();
        }
        if(expectedTotal < 0) {
            const int headerEnd = result.body.indexOf("\r\n\r\n");
            const int contentLengthAt = result.body.indexOf("Content-Length:");
            if(headerEnd >= 0 && contentLengthAt >= 0 && contentLengthAt < headerEnd) {
                const int valueStart = contentLengthAt + 15;
                const int valueEnd = result.body.indexOf("\r\n", valueStart);
                const int contentLength = result.body.substring(valueStart, valueEnd).toInt();
                if(contentLength >= 0) {
                    expectedTotal = headerEnd + 4 + contentLength;
                }
            }
        }
        if(expectedTotal >= 0 && result.length >= static_cast<size_t>(expectedTotal)) {
            break;
        }
        const uint32_t idleMs = millis() - lastDataAt;
        if(!client.connected() && client.available() <= 0 && idleMs > 500) {
            break;
        }
        if(idleMs > (expectedTotal >= 0 ? 8000UL : 5000UL)) {
            break;
        }
        delay(10);
        yield();
    }
    const uint32_t settleUntil = millis() + 450UL;
    while(static_cast<int32_t>(millis() - settleUntil) < 0) {
        while(client.available() > 0) {
            (void)client.read();
        }
        delay(10);
        yield();
    }
    client.stop();

    const int firstSpace = result.body.indexOf(' ');
    if(firstSpace >= 0 && result.body.startsWith("HTTP/")) {
        result.code = result.body.substring(firstSpace + 1, firstSpace + 4).toInt();
    }
    result.ok = result.code >= 200 && result.code < 400;
    if(result.code < 0) {
        result.error = "no status";
    } else if(!result.ok) {
        result.error = String("http ") + String(result.code);
    }
    return result;
}

WifiPhoneHttpResult wifiPhoneHttpGet(const String& path) {
    return wifiPhoneHttpRequest("GET", path);
}

WifiPhoneHttpResult wifiPhoneHttpPost(const String& path, const String& formBody) {
    return wifiPhoneHttpRequest("POST", path, formBody);
}

WifiPhoneHttpResult wifiProbeHttpGet(const String& host, const String& path) {
    WifiPhoneHttpResult result;
    WiFiClient client;
    client.setTimeout(2500);
    IPAddress numericHost;
    const bool hostIsIp = numericHost.fromString(host);
    const bool connected = hostIsIp
                               ? client.connect(numericHost, 80, 5000)
                               : client.connect(host.c_str(), 80, 5000);
    if(!connected) {
        result.error = "connect failed";
        return result;
    }
    client.setNoDelay(true);

    client.print("GET ");
    client.print(path.length() > 0 ? path : String("/"));
    client.print(" HTTP/1.0\r\nHost: ");
    client.print(host);
    client.print("\r\nConnection: close\r\n\r\n");

    const uint32_t startedAt = millis();
    uint32_t lastDataAt = startedAt;
    while((millis() - startedAt) < 20000) {
        while(client.available() > 0) {
            const char ch = static_cast<char>(client.read());
            if(result.body.length() < 4096) {
                result.body += ch;
            }
            ++result.length;
            lastDataAt = millis();
        }
        const uint32_t idleMs = millis() - lastDataAt;
        if(!client.connected() && client.available() <= 0 && idleMs > 500) {
            break;
        }
        if(idleMs > 5000) {
            break;
        }
        delay(10);
        yield();
    }
    const uint32_t settleUntil = millis() + 450UL;
    while(static_cast<int32_t>(millis() - settleUntil) < 0) {
        while(client.available() > 0) {
            (void)client.read();
        }
        delay(10);
        yield();
    }
    client.stop();

    const int firstSpace = result.body.indexOf(' ');
    if(firstSpace >= 0 && result.body.startsWith("HTTP/")) {
        result.code = result.body.substring(firstSpace + 1, firstSpace + 4).toInt();
    }
    result.ok = result.code >= 200 && result.code < 400;
    if(result.code < 0) {
        result.error = "no status";
    } else if(!result.ok) {
        result.error = String("http ") + String(result.code);
    }
    return result;
}

bool waitForPhoneStaConnection(const String& ssid, const String& password, uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    delay(100);
    delay(1800);
    yield();
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    const uint32_t disconnectStartedAt = millis();
    while((millis() - disconnectStartedAt) < 1500 &&
          (WiFi.status() == WL_CONNECTED || WiFi.localIP()[0] != 0)) {
        delay(50);
        yield();
    }
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);

    WifiPhoneApScanResult scanResult;
    const int networkCount = WiFi.scanNetworks(false, true, false, 500, 0);
    for(int i = 0; i < networkCount; ++i) {
        if(WiFi.SSID(i) == ssid) {
            scanResult.found = true;
            scanResult.channel = WiFi.channel(i);
            scanResult.rssi = WiFi.RSSI(i);
            const uint8_t* bssid = WiFi.BSSID(i);
            if(bssid != nullptr) {
                memcpy(scanResult.bssid, bssid, sizeof(scanResult.bssid));
            }
            break;
        }
    }
    printWifiPhoneEvent("scan_controller_ap",
                        scanResult.found,
                        networkCount,
                        networkCount > 0 ? static_cast<size_t>(networkCount) : 0,
                        scanResult.found
                            ? (String("channel=") + String(scanResult.channel) +
                               " rssi=" + String(scanResult.rssi))
                            : String("not found"));

    const char* passphrase = password.length() > 0 ? password.c_str() : nullptr;
    WiFi.scanDelete();
    delay(50);
    const wl_status_t beginStatus = scanResult.found && scanResult.channel > 0
                                        ? WiFi.begin(ssid.c_str(), passphrase, scanResult.channel, scanResult.bssid)
                                        : WiFi.begin(ssid.c_str(), passphrase);
    printWifiPhoneEvent("begin_controller_ap",
                        beginStatus != WL_CONNECT_FAILED,
                        static_cast<int>(beginStatus),
                        0,
                        wifiStatusName(beginStatus));

    const uint32_t startedAt = millis();
    while((millis() - startedAt) < timeoutMs) {
        if(WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0 && WiFi.gatewayIP()[0] != 0) {
            delay(650);
            yield();
            return true;
        }
        delay(250);
        yield();
    }
    return false;
}

void restoreEspNowAfterWifiPhoneTest() {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_MODE_NULL);
    app.currentTransport = LinkTransport::None;
    app.transportOverrideActive = true;
    switchTransportNow(LinkTransport::EspNow);
    requestEspNowDiscovery(true);
}

bool runWifiPhoneProvisionTest(const String& controllerApSsid,
                               const String& homeSsid,
                               const String& homePassword,
                               String* message) {
    if(controllerApSsid.isEmpty() || homeSsid.isEmpty()) {
        if(message != nullptr) {
            *message = "usage: wifi phone <controller_ap_ssid> <home_ssid> <home_password>";
        }
        return false;
    }

    stopWebSocketSession();
    if(remote_wifi_is_active()) {
        remote_wifi_stop();
    }
    stopEspNowSession();
    app.currentTransport = LinkTransport::None;
    app.lastLinkActivityAt = 0;

    const bool staOk = waitForPhoneStaConnection(controllerApSsid, "", 22000);
    String connectDetail = String("ssid=") + controllerApSsid +
                           " status=" + wifiStatusName(WiFi.status()) +
                           " ip=" + WiFi.localIP().toString();
    printWifiPhoneEvent("connect_controller_ap", staOk, 0, 0, connectDetail);
    if(!staOk) {
        restoreEspNowAfterWifiPhoneTest();
        if(message != nullptr) {
            *message = "controller AP connect failed";
        }
        return false;
    }

    auto ensureControllerApConnected = [&]() -> bool {
        if(WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0) {
            return true;
        }
        return waitForPhoneStaConnection(controllerApSsid, "", 12000);
    };
    auto forceControllerApReconnect = [&]() -> bool {
        WiFi.disconnect(false, false);
        delay(800);
        return waitForPhoneStaConnection(controllerApSsid, "", 12000);
    };

    struct EndpointProbe {
        const char* step;
        const char* path;
        const char* needle;
    };
    const EndpointProbe probes[] = {
        {"apple_captive", "/hotspot-detect.html", "SG98"},
    };
    for(const EndpointProbe& probe : probes) {
        if(!ensureControllerApConnected()) {
            restoreEspNowAfterWifiPhoneTest();
            if(message != nullptr) {
                *message = String("reconnect failed before ") + probe.step;
            }
            return false;
        }
        const WifiPhoneHttpResult result = wifiPhoneHttpGet(probe.path);
        printWifiPhoneEvent(probe.step,
                            result.ok,
                            result.code,
                            result.length,
                            result.error.length() ? result.error : String(probe.path),
                            result.body,
                            probe.needle);
        if(!result.ok) {
            restoreEspNowAfterWifiPhoneTest();
            if(message != nullptr) {
                *message = String("http failed at ") + probe.step;
            }
            return false;
        }
    }
    bool scanComplete = false;
    bool scanSawHome = false;
    for(uint8_t attempt = 0; attempt < 8; ++attempt) {
        if(!ensureControllerApConnected()) {
            break;
        }
        const String path = attempt == 0 ? "/api/wifi_scan?force=1" : "/api/wifi_scan";
        const WifiPhoneHttpResult scan = wifiPhoneHttpGet(path);
        scanComplete = scan.ok && scan.body.indexOf("\"scanning\":false") >= 0;
        scanSawHome = scan.body.indexOf(homeSsid) >= 0;
        printWifiPhoneEvent("controller_wifi_scan",
                            scan.ok,
                            scan.code,
                            scan.length,
                            path,
                            scan.body,
                            homeSsid);
        if(!scan.ok) {
            (void)forceControllerApReconnect();
        }
        if(scanComplete) {
            break;
        }
        delay(1400);
        yield();
    }

    const String form = String("action=save&ssid=") + urlEncodeFormValue(homeSsid) +
                        "&password=" + urlEncodeFormValue(homePassword);
    bool saveOk = false;
    for(uint8_t attempt = 0; attempt < 5; ++attempt) {
        if(!ensureControllerApConnected() && !forceControllerApReconnect()) {
            continue;
        }
        const WifiPhoneHttpResult save = wifiPhoneHttpPost("/api/wifi", form);
        printWifiPhoneEvent("save_home_wifi",
                            save.ok,
                            save.code,
                            save.length,
                            String("attempt=") + String(static_cast<unsigned>(attempt + 1)) +
                                " ssid=" + homeSsid,
                            save.body,
                            "saved");
        if(save.ok) {
            saveOk = true;
            break;
        }
        (void)forceControllerApReconnect();
        delay(1000);
        yield();
    }
    if(!saveOk) {
        restoreEspNowAfterWifiPhoneTest();
        if(message != nullptr) {
            *message = "save failed";
        }
        return false;
    }

    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(250);
    WiFi.mode(WIFI_MODE_NULL);
    delay(6500);
    yield();
    bool controllerStaConnected = false;
    for(uint8_t attempt = 0; attempt < 5; ++attempt) {
        if(!waitForPhoneStaConnection(controllerApSsid, "", 10000)) {
            printWifiPhoneEvent("controller_wifi_status",
                                false,
                                -1,
                                0,
                                String("attempt=") + String(static_cast<unsigned>(attempt + 1)) +
                                    " reconnect failed",
                                String(),
                                "\"staConnected\":true");
            delay(2200);
            yield();
            continue;
        }
        const WifiPhoneHttpResult status = wifiPhoneHttpGet("/api/wifi");
        controllerStaConnected = status.ok &&
                                 status.body.indexOf("\"staConnected\":true") >= 0 &&
                                 status.body.indexOf(homeSsid) >= 0;
        printWifiPhoneEvent("controller_wifi_status",
                            status.ok,
                            status.code,
                            status.length,
                            String("attempt=") + String(static_cast<unsigned>(attempt + 1)),
                            status.body,
                            "\"staConnected\":true");
        if(controllerStaConnected) {
            delay(1800);
            yield();
            break;
        }
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        delay(2500);
        yield();
    }

    restoreEspNowAfterWifiPhoneTest();
    if(message != nullptr) {
        *message = controllerStaConnected
                       ? (scanSawHome ? "phone simulation passed" : "passed; home ssid not seen in scan")
                       : "saved; controller sta not confirmed";
    }
    return controllerStaConnected;
}

bool runWifiProbeTest(const String& ssid, const String& host, const String& path, String* message) {
    if(ssid.isEmpty()) {
        if(message != nullptr) {
            *message = "usage: wifi probe <ssid> [host] [path]";
        }
        return false;
    }

    stopWebSocketSession();
    if(remote_wifi_is_active()) {
        remote_wifi_stop();
    }
    stopEspNowSession();
    app.currentTransport = LinkTransport::None;
    app.lastLinkActivityAt = 0;

    const bool staOk = waitForPhoneStaConnection(ssid, "", 22000);
    String connectDetail = String("ssid=") + ssid +
                           " status=" + wifiStatusName(WiFi.status()) +
                           " ip=" + WiFi.localIP().toString() +
                           " gw=" + WiFi.gatewayIP().toString() +
                           " dns=" + WiFi.dnsIP().toString() +
                           " mask=" + WiFi.subnetMask().toString();
    printWifiPhoneEvent("probe_connect", staOk, 0, 0, connectDetail);
    bool ok = staOk;
    if(staOk) {
        const String requestHost = host.length() > 0 ? host : String("captive.apple.com");
        const String requestPath = path.length() > 0 ? path : String("/hotspot-detect.html");
        const WifiPhoneHttpResult result = wifiProbeHttpGet(requestHost, requestPath);
        printWifiPhoneEvent("probe_http",
                            result.ok,
                            result.code,
                            result.length,
                            requestHost + requestPath,
                            result.body);
        ok = result.ok;
    }

    restoreEspNowAfterWifiPhoneTest();
    if(message != nullptr) {
        *message = ok ? "probe passed" : "probe failed";
    }
    return ok;
}

bool runWifiBounceTest(const String& ssid, String* message) {
    if(ssid.isEmpty()) {
        if(message != nullptr) {
            *message = "usage: wifi bounce <ssid>";
        }
        return false;
    }

    stopWebSocketSession();
    if(remote_wifi_is_active()) {
        remote_wifi_stop();
    }
    stopEspNowSession();
    app.currentTransport = LinkTransport::None;
    app.lastLinkActivityAt = 0;

    bool staOk = waitForPhoneStaConnection(ssid, "", 26000);
    if(!staOk) {
        delay(1000);
        staOk = waitForPhoneStaConnection(ssid, "", 26000);
    }
    String connectDetail = String("ssid=") + ssid +
                           " status=" + wifiStatusName(WiFi.status()) +
                           " ip=" + WiFi.localIP().toString() +
                           " gw=" + WiFi.gatewayIP().toString();
    printWifiPhoneEvent("bounce_connect", staOk, 0, 0, connectDetail);

    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(500);
    WiFi.mode(WIFI_MODE_NULL);
    app.currentTransport = LinkTransport::None;
    app.transportOverrideActive = true;
    if(message != nullptr) {
        *message = staOk ? "bounce passed" : "bounce failed";
    }
    return staOk;
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

const esp_partition_t* otaBootPartitionForToken(String token) {
    token.toLowerCase();
    if(token == "next") {
        return esp_ota_get_next_update_partition(nullptr);
    }
    if(token == "running" || token == "current") {
        return esp_ota_get_running_partition();
    }
    if(token == "app0" || token == "ota0" || token == "ota_0") {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    }
    if(token == "app1" || token == "ota1" || token == "ota_1") {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    }
    return nullptr;
}

bool setOtaBootPartition(String target, String* message) {
    const esp_partition_t* partition = otaBootPartitionForToken(target);
    if(partition == nullptr) {
        if(message != nullptr) {
            *message = "usage: api ota boot <next|running|app0|app1> [reboot]";
        }
        return false;
    }

    const esp_err_t err = esp_ota_set_boot_partition(partition);
    if(err != ESP_OK) {
        if(message != nullptr) {
            *message = String("set boot failed err=") + String(static_cast<int>(err));
        }
        return false;
    }

    if(message != nullptr) {
        char offset[12];
        snprintf(offset, sizeof(offset), "0x%lx", static_cast<unsigned long>(partition->address));
        *message = String("boot=") + partition->label + " offset=" + offset + " reboot required";
    }
    return true;
}

void restoreOtaWifi(const RemoteOtaWifiGuard& guard) {
    if(guard.switchedTransport && guard.previousTransport != LinkTransport::WebSocket) {
        switchTransportNow(guard.previousTransport);
    }
}

bool ensureOtaWifi(RemoteOtaWifiGuard* guard, String* message) {
    if(guard == nullptr) {
        return false;
    }
    guard->previousTransport = app.currentTransport;

    if(remote_wifi_sta_connected()) {
        return true;
    }
    if(!remote_wifi_has_saved_network()) {
        if(message != nullptr) {
            *message = "no saved wifi";
        }
        return false;
    }

    if(app.currentTransport != LinkTransport::WebSocket || !remote_wifi_is_active()) {
        switchTransportNow(LinkTransport::WebSocket);
        guard->switchedTransport = true;
    }

    const uint32_t startedAt = millis();
    while(static_cast<uint32_t>(millis() - startedAt) < 30000UL) {
        remote_wifi_poll();
        if(remote_wifi_sta_connected()) {
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

bool downloadOtaImage(String url, String* message) {
    if(url.length() == 0) {
        if(message != nullptr) {
            *message = "usage: api ota download <url> [boot|reboot]";
        }
        return false;
    }

    RemoteOtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        return false;
    }

    sg98_ota_download::Result result;
    const bool ok = sg98_ota_download::downloadToNextSlot(url, &result);
    restoreOtaWifi(guard);
    if(message != nullptr) {
        *message = result.message;
    }
    return ok;
}

bool downloadOtaManifest(String url, String* message) {
    const String resolvedUrl = sg98_ota_source::resolveManifestUrl(url);
    if(resolvedUrl.length() == 0) {
        if(message != nullptr) {
            *message = "usage: api ota manifest <url|latest> [boot|reboot]";
        }
        return false;
    }

    RemoteOtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        return false;
    }

    sg98_ota_download::Result result;
    const bool ok = sg98_ota_download::downloadManifestToNextSlot(resolvedUrl, &result);
    restoreOtaWifi(guard);
    if(message != nullptr) {
        *message = result.message;
    }
    return ok;
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

void printRemoteConsoleDevice() {
    String out;
    out.reserve(384);
    out += "{\"type\":\"device\",\"protocol\":\"sg98-usb-device-v1\",\"role\":\"remote\",\"target\":\"remote\",\"board\":";
    appendJsonQuoted(out, remote_board_info().name);
    out += ",\"mac\":";
    appendJsonQuoted(out, stableRemoteMacAddress());
    out += ",\"firmwareVersion\":";
    appendJsonQuoted(out, sg98_version::release());
    out += ",\"gitSha\":";
    appendJsonQuoted(out, sg98_version::gitSha());
    out += ",\"gitDirty\":";
    out += sg98_version::dirty() ? "true" : "false";
    out += ",\"buildEnv\":";
    appendJsonQuoted(out, sg98_version::buildEnv());
    out += ",\"buildDate\":";
    appendJsonQuoted(out, SG98_BUILD_DATE);
    out += ",\"buildTime\":";
    appendJsonQuoted(out, SG98_BUILD_TIME);
    out += ",\"supportsAppOnlyUpdate\":true";
    out += ",\"applicationOffset\":";
    appendJsonQuoted(out, runningApplicationOffsetHex());
    out += ",\"applicationSize\":";
    out += String(static_cast<unsigned long>(runningApplicationSize()));
    out += ",\"supportsOtaUpdate\":";
    out += nextOtaSize() > 0 ? "true" : "false";
    out += ",\"otaManifestUrl\":";
    appendJsonQuoted(out, sg98_ota_source::latestManifestUrl());
    out += ",\"otaNextOffset\":";
    appendJsonQuoted(out, nextOtaOffsetHex());
    out += ",\"otaNextSize\":";
    out += String(static_cast<unsigned long>(nextOtaSize()));
    out += ",\"transport\":";
    appendJsonQuoted(out, transportName(app.currentTransport));
    out += ",\"apSSID\":";
    appendJsonQuoted(out, remote_wifi_ap_ssid());
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
    Serial.println("[remote-diag] api: api device | api status | api snapshot | api set <menu> <item> <value> | api setrpm <rpm> | api setweight <mg>");
    Serial.println("[remote-diag] api: api ota check [url|latest]");
    Serial.println("[remote-diag] UI input: api ui primary_short|primary_long|secondary_short|dial <-20..20>");
    Serial.println("[remote-diag] api: api ota boot <next|running|app0|app1> [reboot] | api ota download <url> [boot|reboot] | api ota manifest <url|latest> [boot|reboot] | api reboot");
    Serial.println("[remote-diag] api: api action <start|stop|idle|idle_gbw|redetect|calibrate|drive auto|drive jmc|drive rt|drive krs|drive xb130|drive sew|drive tse|jmc_baud <baud>|controller_ota|controller_ota_check|controller_ota_download|controller_ota_apply|reset_network|reset_defaults confirm|peer_scan|peer_clear|ws_scan|audio_probe|audio_click|audio_swipe|audio_sweep|voice>");
    Serial.println("[remote-diag] transport: transport <ws|espnow|auto>");
    Serial.println("[remote-diag] voice: voice status | voice url <http://host:port/command> | voice clear | voice test");
    Serial.println("[remote-diag] peers: peer scan | peer list | peer lock <MAC> | peer clear");
    Serial.println("[remote-diag] wifi: wifi phone <controller_ap_ssid> <home_ssid> <home_password>");
    Serial.println("[remote-diag] wifi: wifi probe <ssid> [host] [path]");
    Serial.println("[remote-diag] wifi: wifi bounce <ssid>");
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

bool applyVoiceIntent(const RemoteVoiceResult& result, String* message) {
    String command = result.command;
    command.trim();
    command.toLowerCase();
    command.replace('-', '_');
    command.replace(' ', '_');

    if(command == "stop" || command == "halt" || command == "cancel") {
        if(!isRunningState()) {
            if(message != nullptr) {
                *message = "already stopped";
            }
            return true;
        }
        const char* expectedState =
            (app.hasStatus && app.status.state == "GRINDING" && app.menuValues[2][SETBUTTONPURGE] != 0)
                ? "PURGING"
                : (isGbwState() ? "IDLE_GBW" : "IDLE");
        const bool ok = queuePendingAction(PendingActionKind::Stop,
                                           PendingActionReflection::State,
                                           nullptr,
                                           expectedState,
                                           0);
        if(message != nullptr) {
            *message = ok ? "stop queued" : "stop send failed";
        }
        return ok;
    }

    if(command == "manual" || command == "manual_mode") {
        if(!stateAllowsModeToggle()) {
            if(message != nullptr) {
                *message = "stop first";
            }
            return false;
        }
        app.preferGbw = false;
        if(app.hasStatus && (app.status.state == "IDLE" || app.status.state == "IDLE_GBW")) {
            app.status.state = "IDLE";
        }
        const bool ok = queuePendingAction(PendingActionKind::Idle,
                                           PendingActionReflection::State,
                                           nullptr,
                                           "IDLE",
                                           0);
        if(message != nullptr) {
            *message = ok ? "manual mode" : "manual send failed";
        }
        return ok;
    }

    if(command == "gbw" || command == "weight" || command == "weight_mode" || command == "grind_by_weight") {
        if(!stateAllowsModeToggle()) {
            if(message != nullptr) {
                *message = "stop first";
            }
            return false;
        }
        app.preferGbw = true;
        if(app.hasStatus && (app.status.state == "IDLE" || app.status.state == "IDLE_GBW")) {
            app.status.state = "IDLE_GBW";
        }
        const bool ok = queuePendingAction(PendingActionKind::IdleGbw,
                                           PendingActionReflection::State,
                                           nullptr,
                                           "IDLE_GBW",
                                           0);
        if(message != nullptr) {
            *message = ok ? "gbw mode" : "gbw send failed";
        }
        return ok;
    }

    if(command == "set_rpm" || command == "rpm") {
        if(!result.hasValue) {
            if(message != nullptr) {
                *message = "rpm value missing";
            }
            return false;
        }
        return remoteConsoleSetTarget(result.value, false, message);
    }

    if(command == "set_weight" || command == "set_dose" || command == "dose") {
        if(!result.hasValue) {
            if(message != nullptr) {
                *message = "weight value missing";
            }
            return false;
        }
        return remoteConsoleSetTarget(result.value, true, message);
    }

    if(command == "home") {
        setScreen(Screen::Home);
        if(message != nullptr) {
            *message = "home";
        }
        return true;
    }

    if(command == "menu" || command == "settings") {
        setScreen(Screen::Main);
        if(message != nullptr) {
            *message = "menu";
        }
        return true;
    }

    if(command == "start") {
#if REMOTE_VOICE_ALLOW_START
        if(!isRunActionAvailable()) {
            if(message != nullptr) {
                *message = "controller offline";
            }
            return false;
        }
        if(isRunningState()) {
            if(message != nullptr) {
                *message = "already running";
            }
            return true;
        }
        const bool gbwMode = isGbwState();
        const bool ok = queuePendingAction(PendingActionKind::Start,
                                           PendingActionReflection::State,
                                           nullptr,
                                           gbwMode ? "GRINDING_GBW" : "GRINDING",
                                           0);
        if(message != nullptr) {
            *message = ok ? "start queued" : "start send failed";
        }
        return ok;
#else
        if(message != nullptr) {
            *message = "voice start disabled";
        }
        return false;
#endif
    }

    if(message != nullptr) {
        *message = String("unknown voice command: ") + command;
    }
    return false;
}

bool runVoiceCommand(String* message, bool showToast) {
    if(remote_voice_endpoint().length() == 0) {
        if(message != nullptr) {
            *message = "set voice url first";
        }
        if(showToast) {
            setToast("Set voice URL first", 1800);
        }
        return false;
    }

    RemoteOtaWifiGuard guard;
    String wifiMessage;
    if(!ensureOtaWifi(&guard, &wifiMessage)) {
        if(message != nullptr) {
            *message = wifiMessage;
        }
        if(showToast) {
            setToast(wifiMessage, 1600);
        }
        return false;
    }

    if(showToast) {
        setToast("Listening...", 1200);
        app.redraw = true;
    }

    RemoteVoiceResult result;
    const bool recognized = remote_voice_recognize(&result);
    restoreOtaWifi(guard);

    if(!recognized) {
        const String errorMessage = result.message[0] != '\0' ? String(result.message) : String(remote_voice_detail());
        if(message != nullptr) {
            *message = errorMessage;
        }
        if(showToast) {
            setToast(errorMessage, 2200);
        }
        return false;
    }

    String actionMessage;
    const bool applied = applyVoiceIntent(result, &actionMessage);
    if(message != nullptr) {
        *message = result.transcript[0] != '\0'
                       ? String(result.transcript) + " -> " + actionMessage
                       : actionMessage;
    }
    if(showToast) {
        setToast(actionMessage, 1800);
    }
    app.lastStatusPollAt = 0;
    app.redraw = true;
    return applied;
}

bool remoteConsoleRunAction(String commandLine, String* message) {
    String action = takeToken(&commandLine);
    action.toLowerCase();
    if(systemOtaBusy() && action != "stop") {
        if(message != nullptr) *message = "system update running";
        return false;
    }
    if(action == "system_ota") {
        confirmSystemOta();
        if(message != nullptr) *message = systemOtaBusy() ? "system update started" : "system update confirmation";
        return true;
    }

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
        LocalDriveSelection selection = LOCAL_DRIVE_AUTO;
        PendingActionKind pendingKind = PendingActionKind::None;
        const char* expectedLabel = nullptr;
        if(forcedDriveForArg(arg0.c_str(), &selection, &pendingKind, &expectedLabel)) {
            setLocalDriveSelection(selection);
            markPersistentStateDirty();
            const bool ok = queuePendingAction(pendingKind,
                                               PendingActionReflection::Drive,
                                               arg0.c_str(),
                                               expectedLabel,
                                               0);
            if(message != nullptr) {
                *message = ok ? "queued" : "send failed";
            }
            return ok;
        }
        if(message != nullptr) {
            *message = "usage: drive auto|jmc|rt|krs|xb130|sew|tse";
        }
        return false;
    }

    if(action == "jmc_baud") {
        if(!activeDriveIsJmc()) {
            if(message != nullptr) {
                *message = "JMC baud unavailable for active drive";
            }
            return false;
        }
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

    if(action == "controller_ota") {
        const bool ok = requestControllerOta(message);
        return ok;
    }

    if(action == "controller_ota_check") {
        const bool ok = requestControllerOtaCheck(message);
        return ok;
    }

    if(action == "controller_ota_download") {
        const bool ok = requestControllerOtaDownload(message);
        return ok;
    }

    if(action == "controller_ota_apply") {
        if(!controllerOtaReady()) {
            if(message != nullptr) {
                *message = "no controller ota ready";
            }
            return false;
        }
        const bool ok = sendAction("ota_apply_ready_reboot");
        if(message != nullptr) {
            *message = ok ? "apply sent" : "send failed";
        }
        return ok;
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

    if(action == "voice") {
        return runVoiceCommand(message, false);
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
    appendJsonQuoted(out, frame.startsWith("WIFI|") ? String("WIFI|[redacted]") : frame);
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

uint32_t traceLoopStep(const char* tag,
                       uint32_t sectionStartUs,
                       uint32_t loopStartUs,
                       const RemoteInputState& input) {
    if(!kLoopLatencyDebug) {
        return sectionStartUs;
    }

    const uint32_t nowUs = micros();
    const uint32_t elapsedUs = nowUs - sectionStartUs;
    if(elapsedUs >= kLoopSectionWarnUs) {
        const bool touchActive = input.touch.pressed || input.touch.justPressed ||
                                 input.touch.justReleased || touchTracker.active;
        Serial.printf("[lag] app.%s step=%luus loop=%luus touch=%u screen=%u edit=%u redraw=%u target=%u transport=%u t=%lu\n",
                      tag,
                      static_cast<unsigned long>(elapsedUs),
                      static_cast<unsigned long>(nowUs - loopStartUs),
                      touchActive ? 1u : 0u,
                      static_cast<unsigned>(app.screen),
                      app.editing ? 1u : 0u,
                      app.redraw ? 1u : 0u,
                      app.targetDirty ? 1u : 0u,
                      static_cast<unsigned>(app.currentTransport),
                      static_cast<unsigned long>(millis()));
    }
    return nowUs;
}

void traceLoopTotal(uint32_t loopStartUs, const RemoteInputState& input, bool yieldTransport) {
    if(!kLoopLatencyDebug) {
        return;
    }

    const uint32_t elapsedUs = micros() - loopStartUs;
    if(elapsedUs < kLoopTotalWarnUs) {
        return;
    }
    const bool touchActive = input.touch.pressed || input.touch.justPressed ||
                             input.touch.justReleased || touchTracker.active;
    Serial.printf("[lag] app.loop total=%luus touch=%u screen=%u edit=%u redraw=%u target=%u transport=%u yield=%u t=%lu\n",
                  static_cast<unsigned long>(elapsedUs),
                  touchActive ? 1u : 0u,
                  static_cast<unsigned>(app.screen),
                  app.editing ? 1u : 0u,
                  app.redraw ? 1u : 0u,
                  app.targetDirty ? 1u : 0u,
                  static_cast<unsigned>(app.currentTransport),
                  yieldTransport ? 1u : 0u,
                  static_cast<unsigned long>(millis()));
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
    if(line.equalsIgnoreCase("device") ||
       line.equalsIgnoreCase("identity") ||
       line.equalsIgnoreCase("api device") ||
       line.equalsIgnoreCase("api identity")) {
        printRemoteConsoleDevice();
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
            app.autoEspNowReturnNotified = false;
            app.webFallbackEspNowUntil = 0;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
            app.preserveWifiForEspNowRecovery = false;
            switchTransportNow(LinkTransport::WebSocket);
        } else if(tail == "espnow" || tail == "esp-now") {
            app.transportOverrideActive = true;
            app.webFallbackEspNowUntil = 0;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
            app.preserveWifiForEspNowRecovery = false;
            switchTransportNow(LinkTransport::EspNow);
            requestEspNowDiscovery(true);
        } else if(tail == "auto") {
            app.transportOverrideActive = false;
            app.webFallbackEspNowUntil = 0;
            app.lastWebFallbackRecoveryHintAt = 0;
            app.webFallbackWifiReleaseAt = 0;
            app.preserveWifiForEspNowRecovery = false;
            switchTransportNow(initialTransport());
        } else {
            ok = false;
            message = "usage: transport ws|espnow|auto";
        }
        printRemoteConsoleAck(line, ok, message);
        return;
    }

    if(cmd == "voice") {
        String sub = takeToken(&tail);
        sub.toLowerCase();
        if(sub.length() == 0 || sub == "status") {
            const String endpoint = remote_voice_endpoint();
            printRemoteConsoleAck(line,
                                  endpoint.length() > 0,
                                  endpoint.length() > 0 ? endpoint : String("voice url not set"));
            return;
        }
        if(sub == "url") {
            String url = tail;
            url.trim();
            const bool ok = remote_voice_set_endpoint(url);
            printRemoteConsoleAck(line, ok, ok ? String("saved") : String(remote_voice_detail()));
            return;
        }
        if(sub == "clear") {
            remote_voice_clear_endpoint();
            printRemoteConsoleAck(line, true, "cleared");
            return;
        }
        if(sub == "test") {
            String message;
            const bool ok = runVoiceCommand(&message, true);
            printRemoteConsoleAck(line, ok, message);
            return;
        }
        printRemoteConsoleAck(line, false, "usage: voice status|url <http://host:port/command>|clear|test");
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
                const uint8_t channel = activeEspNowPeerChannel();
                if(channel != 0) {
                    (void)setEspNowChannel(channel);
                }
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

    if(cmd == "wifi") {
        String sub = takeToken(&tail);
        sub.toLowerCase();
        if(sub == "phone") {
            const String controllerApSsid = takeToken(&tail);
            const String homeSsid = takeToken(&tail);
            const String homePassword = takeToken(&tail);
            String message;
            const bool ok = runWifiPhoneProvisionTest(controllerApSsid, homeSsid, homePassword, &message);
            printRemoteConsoleAck(line, ok, message);
            return;
        }
        if(sub == "probe") {
            const String ssid = takeToken(&tail);
            const String host = takeToken(&tail);
            const String path = takeToken(&tail);
            String message;
            const bool ok = runWifiProbeTest(ssid, host, path, &message);
            printRemoteConsoleAck(line, ok, message);
            return;
        }
        if(sub == "bounce") {
            const String ssid = takeToken(&tail);
            String message;
            const bool ok = runWifiBounceTest(ssid, &message);
            printRemoteConsoleAck(line, ok, message);
            return;
        }
        printRemoteConsoleAck(line,
                              false,
                              "usage: wifi phone <controller_ap_ssid> <home_ssid> <home_password> | wifi probe <ssid> [host] [path] | wifi bounce <ssid>");
        return;
    }

    if(cmd == "api") {
        String sub = takeToken(&tail);
        sub.toLowerCase();
        if(sub == "ui") {
            String event = takeToken(&tail);
            event.toLowerCase();
            RemoteInputState input = {};
            bool valid = true;
            if(event == "primary_short") input.primaryShortPress = true;
            else if(event == "primary_long") input.primaryLongPress = true;
            else if(event == "secondary_short") input.secondaryShortPress = true;
            else if(event == "dial") {
                int32_t delta = 0;
                valid = parseInt32Token(takeToken(&tail), &delta) && delta != 0 && delta >= -20 && delta <= 20;
                input.dialDelta = delta;
            } else valid = false;
            tail.trim();
            valid = valid && tail.length() == 0;
            if(!valid || g_consoleUiInputPending || controllerOtaActionBusy() || app.flashUntil > millis()) {
                printRemoteConsoleAck(line, false, valid ? "UI busy" : "usage: api ui primary_short|primary_long|secondary_short|dial <-20..20>");
                return;
            }
            g_consoleUiInput = input;
            g_consoleUiInputPending = true;
            printRemoteConsoleAck(line, true, "UI input queued");
            return;
        }
        if(sub == "device" || sub == "identity") {
            printRemoteConsoleDevice();
            return;
        }
        if(sub == "ota") {
            String action = takeToken(&tail);
            action.toLowerCase();
            if(action == "check") {
                const String url = sg98_ota_source::resolveManifestUrl(takeToken(&tail));
                const bool queued = remote_wifi_sta_connected() &&
                    queueBackgroundNetwork(BackgroundNetworkKind::OtaManifest, url);
                if(queued) app.otaUpdate.lastAttemptAt = millis();
                printRemoteConsoleAck(line, queued, queued ? "check queued" : "network busy or wifi offline");
                return;
            }
            if(action == "boot") {
                const String target = takeToken(&tail);
                String rebootToken = takeToken(&tail);
                rebootToken.toLowerCase();
                String message;
                const bool ok = setOtaBootPartition(target, &message);
                printRemoteConsoleAck(line, ok, message);
                if(ok && rebootToken == "reboot") {
                    delay(150);
                    Serial.flush();
                    ESP.restart();
                }
                return;
            }
            if(action == "download") {
                const String url = takeToken(&tail);
                String bootToken = takeToken(&tail);
                bootToken.toLowerCase();
                String message;
                const bool ok = downloadOtaImage(url, &message);
                printRemoteConsoleAck(line, ok, message);
                if(ok && (bootToken == "boot" || bootToken == "reboot")) {
                    String bootMessage;
                    const bool bootOk = setOtaBootPartition("next", &bootMessage);
                    printRemoteConsoleAck(line, bootOk, bootMessage);
                    if(bootOk && bootToken == "reboot") {
                        delay(150);
                        Serial.flush();
                        ESP.restart();
                    }
                }
                return;
            }
            if(action == "manifest") {
                const String url = takeToken(&tail);
                String bootToken = takeToken(&tail);
                bootToken.toLowerCase();
                String message;
                const bool ok = downloadOtaManifest(url, &message);
                printRemoteConsoleAck(line, ok, message);
                if(ok && (bootToken == "boot" || bootToken == "reboot")) {
                    String bootMessage;
                    const bool bootOk = setOtaBootPartition("next", &bootMessage);
                    printRemoteConsoleAck(line, bootOk, bootMessage);
                    if(bootOk && bootToken == "reboot") {
                        delay(150);
                        Serial.flush();
                        ESP.restart();
                    }
                }
                return;
            }
            if(action == "reboot") {
                printRemoteConsoleAck(line, true, "rebooting");
                delay(150);
                Serial.flush();
                ESP.restart();
                return;
            }
            printRemoteConsoleAck(line, false, "usage: api ota boot <next|running|app0|app1> [reboot] | api ota download <url> [boot|reboot] | api ota manifest <url|latest> [boot|reboot]");
            return;
        }
        if(sub == "reboot") {
            printRemoteConsoleAck(line, true, "rebooting");
            delay(150);
            Serial.flush();
            ESP.restart();
            return;
        }
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
        printRemoteConsoleAck(line, false, "usage: api device|status|snapshot|set|setrpm|setweight|action");
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
        remote_voice_init();
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
    if(g_statePrefsReady && g_statePrefs.getBool("sysPending", false)) {
        g_systemOta.expectedVersion = g_statePrefs.getString("sysVersion", "");
        g_systemOta.expectedSha = g_statePrefs.getString("sysSha", "");
        const uint8_t transport = g_statePrefs.getUChar("sysTransport", static_cast<uint8_t>(startupTransport));
        if(transport == static_cast<uint8_t>(LinkTransport::EspNow) || transport == static_cast<uint8_t>(LinkTransport::WebSocket)) {
            startupTransport = static_cast<LinkTransport>(transport);
        }
        g_systemOta.previousTransport = startupTransport;
        g_systemOta.previousOverride = g_statePrefs.getBool("sysOverride", false);
        app.screen = Screen::About;
        if(g_systemOta.expectedVersion == sg98_version::release() &&
           systemOtaShaMatches(g_systemOta.expectedSha, sg98_version::gitSha())) {
            g_systemOta.flow.enter(sg98_system_ota::Stage::ControllerVerify, millis());
            g_systemOta.remoteBootVerificationPending = true;
            g_systemOta.detail = "Remote verified; reconnecting controller";
            app.transportOverrideActive = true;
        } else {
            g_systemOta.flow.enter(sg98_system_ota::Stage::Failed, millis());
            g_systemOta.detail = "Remote release verification failed";
            g_statePrefs.remove("sysPending");
        }
    }
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
    if(g_systemOta.remoteInstallRequested) {
        serviceSystemRemoteInstall();
        return;
    }
    if(g_remoteOtaRequested) {
        // Run TLS/flash work after the input callback returns, on a fresh loop frame.
        serviceRemoteOtaUpdate();
        return;
    }
    RemoteInputState input = {};
    const uint32_t now = millis();
    const uint32_t loopStartUs = micros();
    uint32_t sectionStartUs = loopStartUs;
    remote_board_poll_input(&input);
    sectionStartUs = traceLoopStep("board_poll", sectionStartUs, loopStartUs, input);
    if(input.displayRotationChanged) {
        touchTracker = {};
        app.redraw = true;
    }
    remote_audio_service();
    sectionStartUs = traceLoopStep("audio", sectionStartUs, loopStartUs, input);
    remoteConsolePoll();
    if(g_consoleUiInputPending) {
        // Inject before the normal activity, navigation, and confirmation handlers.
        input.primaryShortPress = input.primaryShortPress || g_consoleUiInput.primaryShortPress;
        input.primaryLongPress = input.primaryLongPress || g_consoleUiInput.primaryLongPress;
        input.secondaryShortPress = input.secondaryShortPress || g_consoleUiInput.secondaryShortPress;
        input.dialDelta += g_consoleUiInput.dialDelta;
        g_consoleUiInputPending = false;
    }
    updateInputDiagnostics(input, now);
    sectionStartUs = traceLoopStep("console_poll", sectionStartUs, loopStartUs, input);
    serviceMdnsShutdown();
    serviceBackgroundNetworkResults();
    serviceControllerOtaUi(now);

    const bool hasControlInput =
        input.dialDelta != 0 || input.primaryPressed || input.primaryLongPress || input.primaryShortPress ||
        input.secondaryPressed || input.secondaryLongPress || input.secondaryShortPress ||
        input.touch.pressed || input.touch.justPressed || input.touch.justReleased;
    const bool revealEligible = !systemOtaBusy() && !controllerOtaActionBusy() &&
                                app.flashUntil <= now && app.screen == Screen::Home && !app.editing &&
                                !app.targetDirty && !pendingControllerSyncActive() && app.hasControllerStatus &&
                                (app.controllerStatus.state == "IDLE" || app.controllerStatus.state == "IDLE_GBW");
    const bool revealWasActive = g_connectionReveal.active;
    const bool freshStatus = app.lastStatusAt != 0 && now - app.lastStatusAt < remote_config::kLinkTimeoutMs;
    if(g_connectionReveal.observe(freshStatus, revealEligible && !hasControlInput, now)) {
        if(app.localUiSounds != 0) remote_audio_play(RemoteAudioTone::Reveal);
        app.redraw = true;
    }
    g_connectionReveal.service(revealEligible, hasControlInput, now);
    if(g_connectionReveal.active && now - g_connectionRevealLastFrame >= 40) {
        g_connectionRevealLastFrame = now;
        app.redraw = true;
    }
    if(revealWasActive && !g_connectionReveal.active) {
        remote_audio_cancel_reveal();
        app.redraw = true;
    }
    if(app.dockClockActive && hasControlInput) {
        exitDockClock();
        markUserActivity(now);
        app.lastControlActivityAt = now;
        input.dialDelta = 0;
        input.primaryPressed = false;
        input.primaryShortPress = false;
        input.primaryLongPress = false;
        input.secondaryPressed = false;
        input.secondaryShortPress = false;
        input.secondaryLongPress = false;
        input.touch = {};
    }
    const bool systemOtaConfirmationArmed =
        app.screen == Screen::About && dangerIsArmed(Screen::About, 2);
    if((systemOtaBusy() || (controllerOtaActionBusy() && !systemOtaConfirmationArmed)) && hasControlInput) {
        markUserActivity(now);
        app.lastControlActivityAt = now;
        input.dialDelta = 0;
        input.primaryPressed = false;
        input.primaryShortPress = false;
        input.primaryLongPress = false;
        input.secondaryPressed = false;
        input.secondaryShortPress = false;
        input.secondaryLongPress = false;
        input.touch = {};
        setToast(systemOtaBusy() ? "System update running" : "Controller OTA running", 900);
    }
    if(hasControlInput) {
        app.lastControlActivityAt = now;
        app.motionHomeCandidateAt = 0;
    }
    if(input.powerStatusChanged) {
        refreshBatteryStatus(true);
        sectionStartUs = traceLoopStep("battery_irq", sectionStartUs, loopStartUs, input);
    }

    if(input.dialDelta != 0 || input.primaryPressed ||
       input.primaryLongPress || input.primaryShortPress ||
       input.secondaryPressed || input.secondaryLongPress || input.secondaryShortPress ||
       input.touch.pressed || input.touch.justPressed || input.touch.justReleased) {
        markUserActivity(now);
    }

    if(input.motionDetected && handleMotionHomeGesture(now)) {
        markUserActivity(now);
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
        sectionStartUs = traceLoopStep("primary_long", sectionStartUs, loopStartUs, input);
    }
    if(input.primaryShortPress) {
        handlePrimaryShortPress();
        sectionStartUs = traceLoopStep("primary_short", sectionStartUs, loopStartUs, input);
    }
    if(input.secondaryLongPress) {
        handleSecondaryLongPress();
        sectionStartUs = traceLoopStep("secondary_long", sectionStartUs, loopStartUs, input);
    }
    if(input.secondaryShortPress) {
        handleSecondaryShortPress();
        sectionStartUs = traceLoopStep("secondary_short", sectionStartUs, loopStartUs, input);
    }

    handleTouchInput(input.touch);
    sectionStartUs = traceLoopStep("touch", sectionStartUs, loopStartUs, input);
    handleDialDelta(conditionHardwareDialForMenu(applyDialDirection(input.dialDelta)));
    sectionStartUs = traceLoopStep("dial", sectionStartUs, loopStartUs, input);
    commitTargetIfDue();
    sectionStartUs = traceLoopStep("target_commit", sectionStartUs, loopStartUs, input);
    servicePersistentSnapshotSave();
    sectionStartUs = traceLoopStep("persist", sectionStartUs, loopStartUs, input);
    serviceSafetyHeartbeat();
    sectionStartUs = traceLoopStep("heartbeat", sectionStartUs, loopStartUs, input);
    servicePressureProfiler(now);
    sectionStartUs = traceLoopStep("pressure", sectionStartUs, loopStartUs, input);
    serviceControllerEnvironmentSync(now);
    sectionStartUs = traceLoopStep("env_sync", sectionStartUs, loopStartUs, input);
    serviceDockClockTime(now);
    sectionStartUs = traceLoopStep("clock_sync", sectionStartUs, loopStartUs, input);
    serviceBackgroundDockClockWeather(now);
    sectionStartUs = traceLoopStep("weather_sync", sectionStartUs, loopStartUs, input);
    serviceFirmwareUpdateCheck(now);
    sectionStartUs = traceLoopStep("ota_check", sectionStartUs, loopStartUs, input);
    if(!systemOtaBusy()) serviceDockClock(now);
    sectionStartUs = traceLoopStep("dock_clock", sectionStartUs, loopStartUs, input);
    const bool yieldTransport = !systemOtaBusy() && shouldYieldTransportToUi();
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
            sectionStartUs = traceLoopStep("manage_transport", sectionStartUs, loopStartUs, input);
            servicePendingControllerSync();
            sectionStartUs = traceLoopStep("pending_sync", sectionStartUs, loopStartUs, input);
            serviceEspNowFirstPolicy();
            sectionStartUs = traceLoopStep("espnow_policy", sectionStartUs, loopStartUs, input);
            handleSendCompletion();
            sectionStartUs = traceLoopStep("send_completion", sectionStartUs, loopStartUs, input);
            updateSyncFromStatusFallback();
            sectionStartUs = traceLoopStep("sync_fallback", sectionStartUs, loopStartUs, input);
            app.lastTransportServiceAt = now;
        }
        if((now - app.lastPollingServiceAt) >= pollingServiceInterval) {
            syncPolling();
            sectionStartUs = traceLoopStep("sync_polling", sectionStartUs, loopStartUs, input);
            refreshBatteryStatus(false);
            sectionStartUs = traceLoopStep("battery_poll", sectionStartUs, loopStartUs, input);
            app.lastPollingServiceAt = now;
        }
    } else {
        if(yieldTransport) {
            app.transportYieldedToUi = true;
        }
        refreshBatteryStatus(false);
        sectionStartUs = traceLoopStep("battery_yield", sectionStartUs, loopStartUs, input);
    }
    serviceSystemOta();
    remote_board_apply_indicator(currentIndicator());
    sectionStartUs = traceLoopStep("indicator", sectionStartUs, loopStartUs, input);
    updateHomeWheelAnimation();
    sectionStartUs = traceLoopStep("home_anim", sectionStartUs, loopStartUs, input);
    updateMenuRollAnimation();
    sectionStartUs = traceLoopStep("menu_anim", sectionStartUs, loopStartUs, input);
    serviceDeferredHomeStatusRedraw();
    sectionStartUs = traceLoopStep("deferred_status", sectionStartUs, loopStartUs, input);
    serviceNetworkStatusRedraw();
    sectionStartUs = traceLoopStep("network_curve", sectionStartUs, loopStartUs, input);
    serviceFlashScreen(now);
    sectionStartUs = traceLoopStep("flash_screen", sectionStartUs, loopStartUs, input);
    remoteConsoleMaybeStream();
    sectionStartUs = traceLoopStep("console_stream", sectionStartUs, loopStartUs, input);
    enterIdleSleepIfNeeded();
    sectionStartUs = traceLoopStep("idle_sleep", sectionStartUs, loopStartUs, input);

    if(app.redraw) {
        redrawUi();
        sectionStartUs = traceLoopStep("redraw", sectionStartUs, loopStartUs, input);
    }

    traceLoopTotal(loopStartUs, input, yieldTransport);
    remote_runtime_wait_for_input_or_timeout(computeLoopWaitMs(millis(), yieldTransport));
}
