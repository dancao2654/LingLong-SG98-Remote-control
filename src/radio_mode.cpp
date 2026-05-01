#include "radio_mode.h"

#include "controller_env.h"
#include "controller_board.h"
#include "display.h"
#include "espnow_control.h"
#include "main.h"
#include "wifi_ui.h"

namespace {
constexpr unsigned long HEADLESS_ESPNOW_BOOT_RECOVERY_MS = 60000;
constexpr unsigned long HEADLESS_ESPNOW_REMOTE_LOST_MS = 60000;
constexpr unsigned long ESPNOW_LINK_HEALTHY_MS = 5000;
constexpr unsigned long HOME_WIFI_SYNC_GRACE_MS = 120000;
constexpr unsigned long INTERNET_SYNC_GRACE_MS = 45000;
constexpr unsigned long WIFI_SESSION_IDLE_TIMEOUT_MS = 600000;
constexpr unsigned long CAPTIVE_DONE_CLOSE_GRACE_MS = 7000;
constexpr unsigned long INTERNET_SYNC_STARTUP_DELAY_MS = 3500;
constexpr unsigned long INTERNET_SYNC_INTERVAL_MS = 60UL * 60UL * 1000UL;
constexpr unsigned long INTERNET_SYNC_RETRY_MS = 10UL * 60UL * 1000UL;

RadioMode currentMode = RADIO_MODE_WEB;
bool initialized = false;
bool espnowRecoveryWatchActive = false;
bool webRecoveryActive = false;
bool internetSyncPending = false;
bool internetSyncActive = false;
unsigned long espnowRecoveryWatchStartedAt = 0;
unsigned long homeWifiOnlineSince = 0;
unsigned long captiveDoneMarkedAt = 0;
unsigned long nextInternetSyncAt = 0;
unsigned long internetSyncStartedAt = 0;

bool isMenuOrIdleState(States currentState) {
    switch(currentState) {
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

void clearEspNowRecoveryWatch() {
    espnowRecoveryWatchActive = false;
    espnowRecoveryWatchStartedAt = 0;
}

void startEspNowRecoveryWatchIfNeeded() {
    webRecoveryActive = false;
    clearEspNowRecoveryWatch();
    if(!controller_board_requires_remote_recovery() || currentMode != RADIO_MODE_ESPNOW) {
        return;
    }

    espnowRecoveryWatchActive = true;
    espnowRecoveryWatchStartedAt = millis();
    Serial.printf("[radio] headless ESP-NOW recovery window=%lus\n",
                  static_cast<unsigned long>(HEADLESS_ESPNOW_BOOT_RECOVERY_MS / 1000));
}

void noteEspNowRecoverySatisfiedIfNeeded() {
    const unsigned long lastCommandAt = espnow_control_last_command_rx_at();
    if(!espnowRecoveryWatchActive ||
       lastCommandAt == 0 ||
       lastCommandAt < espnowRecoveryWatchStartedAt) {
        return;
    }

    clearEspNowRecoveryWatch();
    Serial.println("[radio] headless ESP-NOW remote seen; staying on ESP-NOW");
}

void switchToWebRecoverySession() {
    if(currentMode != RADIO_MODE_ESPNOW) {
        return;
    }

    clearEspNowRecoveryWatch();
    espnow_control_stop();
    const bool webOk = wifi_ui_start();
    if(webOk) {
        currentMode = RADIO_MODE_WEB;
        Menu1[Menu1Items::RADIOMODE].value = currentMode;
        webRecoveryActive = true;
        disp_updateRequired = true;
        Serial.println("[radio] headless ESP-NOW silent; Wi-Fi recovery active");
        return;
    }

    if(espnow_control_start()) {
        currentMode = RADIO_MODE_ESPNOW;
        startEspNowRecoveryWatchIfNeeded();
        Serial.println("[radio] WEB recovery failed; restored ESP-NOW");
    }
}

void serviceEspNowRecoveryWatch() {
    if(!controller_board_requires_remote_recovery() || currentMode != RADIO_MODE_ESPNOW) {
        return;
    }

    noteEspNowRecoverySatisfiedIfNeeded();

    const unsigned long now = millis();
    const unsigned long lastCommandAt = espnow_control_last_command_rx_at();
    if(espnowRecoveryWatchActive &&
       now - espnowRecoveryWatchStartedAt >= HEADLESS_ESPNOW_BOOT_RECOVERY_MS) {
        switchToWebRecoverySession();
        return;
    }
    if(!espnowRecoveryWatchActive &&
       lastCommandAt != 0 &&
       now - lastCommandAt >= HEADLESS_ESPNOW_REMOTE_LOST_MS) {
        switchToWebRecoverySession();
    }
}

bool espNowLinkHealthy() {
    const unsigned long lastCommandAt = espnow_control_last_command_rx_at();
    return lastCommandAt != 0 && (millis() - lastCommandAt) <= ESPNOW_LINK_HEALTHY_MS;
}

bool localEnvironmentSynced() {
    return controller_env_time_valid() || controller_env_weather_valid();
}

bool localEnvironmentSyncedSince(unsigned long since) {
    const uint32_t ntpAt = controller_env_last_ntp_sync_ms();
    const uint32_t weatherAt = controller_env_last_weather_fetch_ms();
    return (ntpAt != 0 && ntpAt >= since) ||
           (weatherAt != 0 && weatherAt >= since);
}

bool homeWifiSetupSynced() {
    if(!wifi_ui_home_setup_online()) {
        return false;
    }
    if(internetSyncActive && internetSyncStartedAt != 0) {
        return localEnvironmentSyncedSince(internetSyncStartedAt);
    }
    return localEnvironmentSynced();
}

bool homeWifiSetupReadyByTimeout() {
    if(!wifi_ui_home_setup_online()) {
        homeWifiOnlineSince = 0;
        return false;
    }
    if(homeWifiOnlineSince == 0) {
        homeWifiOnlineSince = millis();
        return false;
    }
    const unsigned long graceMs = internetSyncActive ? INTERNET_SYNC_GRACE_MS : HOME_WIFI_SYNC_GRACE_MS;
    return (millis() - homeWifiOnlineSince) >= graceMs;
}

const int32_t* radioModeUserValues(uint8_t* count) {
    static const int32_t kFullControllerModes[] = {
        RADIO_MODE_WEB,
        RADIO_MODE_ESPNOW,
        RADIO_MODE_OFF,
    };
    static const int32_t kHeadlessControllerModes[] = {
        RADIO_MODE_WEB,
        RADIO_MODE_ESPNOW,
    };
    if(controller_board_requires_remote_recovery()) {
        if(count != nullptr) *count = sizeof(kHeadlessControllerModes) / sizeof(kHeadlessControllerModes[0]);
        return kHeadlessControllerModes;
    }
    if(count != nullptr) *count = sizeof(kFullControllerModes) / sizeof(kFullControllerModes[0]);
    return kFullControllerModes;
}

void closeWebBridgeIfReady() {
    if(currentMode != RADIO_MODE_WEB) {
        homeWifiOnlineSince = 0;
        captiveDoneMarkedAt = 0;
        internetSyncActive = false;
        internetSyncStartedAt = 0;
        wifi_ui_set_captive_setup_complete(false);
        return;
    }

    const bool internetSynced = homeWifiSetupSynced();
    const bool syncTimedOut = !internetSynced && homeWifiSetupReadyByTimeout();
    const bool setupReady = internetSynced || syncTimedOut;
    const bool idleTimedOut = wifi_ui_web_idle_for(WIFI_SESSION_IDLE_TIMEOUT_MS);
    const bool shouldAutoPark = webRecoveryActive || internetSyncActive || idleTimedOut;
    if(!setupReady && !idleTimedOut) {
        captiveDoneMarkedAt = 0;
        wifi_ui_set_captive_setup_complete(false);
        return;
    }

    if(setupReady) {
        if(captiveDoneMarkedAt == 0) {
            captiveDoneMarkedAt = millis();
            wifi_ui_set_captive_setup_complete(true);
            disp_updateRequired = true;
            Serial.printf("[radio] Wi-Fi setup ready; captive close grace=%lus\n",
                          static_cast<unsigned long>(CAPTIVE_DONE_CLOSE_GRACE_MS / 1000));
            return;
        }
        if(!shouldAutoPark) {
            return;
        }
        if(wifi_ui_ap_station_count() > 0 &&
           millis() - captiveDoneMarkedAt < CAPTIVE_DONE_CLOSE_GRACE_MS) {
            return;
        }
    } else {
        wifi_ui_set_captive_setup_complete(false);
    }

    Menu1[Menu1Items::RADIOMODE].value = RADIO_MODE_ESPNOW;
    wifi_ui_stop();
    espnow_control_stop();
    if(espnow_control_start()) {
        currentMode = RADIO_MODE_ESPNOW;
        webRecoveryActive = false;
        internetSyncActive = false;
        internetSyncStartedAt = 0;
        internetSyncPending = true;
        nextInternetSyncAt = millis() + (internetSynced ? INTERNET_SYNC_INTERVAL_MS : INTERNET_SYNC_RETRY_MS);
        homeWifiOnlineSince = 0;
        captiveDoneMarkedAt = 0;
        startEspNowRecoveryWatchIfNeeded();
        disp_updateRequired = true;
        Serial.printf("[radio] Wi-Fi %s; parked in ESP-NOW\n",
                      setupReady ? (internetSynced ? "internet sync complete" : "setup grace complete") : "idle timeout");
        return;
    }

    if(wifi_ui_start()) {
        currentMode = RADIO_MODE_WEB;
        Menu1[Menu1Items::RADIOMODE].value = RADIO_MODE_WEB;
        webRecoveryActive = true;
        internetSyncActive = false;
        internetSyncStartedAt = 0;
        captiveDoneMarkedAt = 0;
        Serial.println("[radio] ESP-NOW park failed; Wi-Fi remains active");
    }
}

void startInternetSyncIfReady() {
    if(!internetSyncPending) {
        return;
    }
    if(currentMode == RADIO_MODE_OFF || !wifi_ui_has_saved_network()) {
        internetSyncPending = false;
        return;
    }
    if(controller_board_requires_remote_recovery()) {
        internetSyncPending = false;
        return;
    }
    if(currentMode != RADIO_MODE_ESPNOW || !(state == IDLE || state == IDLE_GBW)) {
        return;
    }
    if(static_cast<int32_t>(millis() - nextInternetSyncAt) < 0) {
        return;
    }

    internetSyncPending = false;
    clearEspNowRecoveryWatch();
    espnow_control_stop();
    if(wifi_ui_start()) {
        currentMode = RADIO_MODE_WEB;
        Menu1[Menu1Items::RADIOMODE].value = RADIO_MODE_WEB;
        webRecoveryActive = false;
        internetSyncActive = true;
        internetSyncStartedAt = millis();
        captiveDoneMarkedAt = 0;
        disp_updateRequired = true;
        Serial.println("[radio] internet maintenance sync active; will park back to ESP-NOW");
        return;
    }

    if(espnow_control_start()) {
        currentMode = RADIO_MODE_ESPNOW;
        internetSyncPending = true;
        nextInternetSyncAt = millis() + INTERNET_SYNC_RETRY_MS;
        startEspNowRecoveryWatchIfNeeded();
        disp_updateRequired = true;
        Serial.println("[radio] internet maintenance sync failed; restored ESP-NOW");
    }
}
}

void radio_mode_init() {
    wifi_ui_init();
    const RadioMode bootMode = radio_mode_from_value(Menu1[Menu1Items::RADIOMODE].value);
    if(bootMode == RADIO_MODE_WEB || bootMode == RADIO_MODE_DUAL) {
        Menu1[Menu1Items::RADIOMODE].value = RADIO_MODE_ESPNOW;
    }
    initialized = false;
    radio_mode_apply(true);
    internetSyncPending = bootMode != RADIO_MODE_OFF && wifi_ui_has_saved_network();
    nextInternetSyncAt = millis() + INTERNET_SYNC_STARTUP_DELAY_MS;
}

void radio_mode_poll() {
    startInternetSyncIfReady();
    if(radio_mode_is_web() || radio_mode_is_dual() || wifi_ui_is_active()) {
        wifi_ui_poll();
        closeWebBridgeIfReady();
    }
    if(radio_mode_is_espnow() || radio_mode_is_dual() || espnow_control_is_active()) {
        espnow_control_poll();
        serviceEspNowRecoveryWatch();
    }
}

bool radio_mode_apply(bool force) {
    const RadioMode desiredMode = radio_mode_desired();
    const RadioMode previousMode = currentMode;
    if(initialized && !force && desiredMode == currentMode) {
        if(desiredMode == RADIO_MODE_WEB || desiredMode == RADIO_MODE_DUAL) {
            webRecoveryActive = false;
            internetSyncActive = false;
            internetSyncStartedAt = 0;
        }
        return true;
    }

    if(initialized || force) {
        clearEspNowRecoveryWatch();
        webRecoveryActive = false;
        internetSyncActive = false;
        internetSyncStartedAt = 0;
        if(desiredMode == RADIO_MODE_OFF) {
            internetSyncPending = false;
            nextInternetSyncAt = 0;
        }
        wifi_ui_stop();
        espnow_control_stop();
    }

    bool ok = false;
    if(desiredMode == RADIO_MODE_OFF) {
        ok = true;
    } else if(desiredMode == RADIO_MODE_WEB) {
        ok = wifi_ui_start();
    } else if(desiredMode == RADIO_MODE_DUAL) {
        ok = wifi_ui_start() && espnow_control_start(true);
    } else {
        ok = espnow_control_start();
    }

    if(ok) {
        currentMode = desiredMode == RADIO_MODE_DUAL ? RADIO_MODE_WEB : desiredMode;
        initialized = true;
        disp_updateRequired = true;
        if(currentMode == RADIO_MODE_ESPNOW) {
            startEspNowRecoveryWatchIfNeeded();
        }
        Serial.printf("[radio] mode=%s\n", radio_mode_label(currentMode));
        return true;
    }

    if(desiredMode == RADIO_MODE_ESPNOW) {
        if(wifi_ui_start()) {
            currentMode = RADIO_MODE_WEB;
            initialized = true;
            disp_updateRequired = true;
            webRecoveryActive = controller_board_requires_remote_recovery();
            Serial.println("[radio] esp-now start failed, restored WEB mode");
        }
    } else if(desiredMode == RADIO_MODE_DUAL) {
        if(wifi_ui_is_active() || wifi_ui_start()) {
            currentMode = RADIO_MODE_WEB;
            initialized = true;
            disp_updateRequired = true;
            Serial.println("[radio] dual start failed, restored WEB mode");
        }
    } else if(previousMode == RADIO_MODE_ESPNOW && espnow_control_start()) {
        currentMode = RADIO_MODE_ESPNOW;
        initialized = true;
        disp_updateRequired = true;
        startEspNowRecoveryWatchIfNeeded();
        Serial.println("[radio] web start failed, restored ESP-NOW mode");
    }

    return false;
}

bool radio_mode_can_switch_now() {
    return isMenuOrIdleState(state);
}

RadioMode radio_mode_current() {
    return currentMode;
}

RadioMode radio_mode_desired() {
    return radio_mode_from_value(radio_mode_sanitize_value(Menu1[Menu1Items::RADIOMODE].value));
}

RadioMode radio_mode_from_value(int32_t value) {
    if(value == RADIO_MODE_ESPNOW) {
        return RADIO_MODE_ESPNOW;
    }
    if(value == RADIO_MODE_OFF) {
        return RADIO_MODE_OFF;
    }
    if(value == RADIO_MODE_DUAL) {
        return RADIO_MODE_DUAL;
    }
    return RADIO_MODE_WEB;
}

int32_t radio_mode_sanitize_value(int32_t value) {
    const RadioMode mode = radio_mode_from_value(value);
    if(mode == RADIO_MODE_DUAL) {
        return RADIO_MODE_WEB;
    }
    if(controller_board_requires_remote_recovery() && mode == RADIO_MODE_OFF) {
        return RADIO_MODE_WEB;
    }
    return mode;
}

uint8_t radio_mode_user_value_count() {
    uint8_t count = 0;
    radioModeUserValues(&count);
    return count;
}

int32_t radio_mode_user_value_at(uint8_t index) {
    uint8_t count = 0;
    const int32_t* values = radioModeUserValues(&count);
    if(count == 0) {
        return RADIO_MODE_WEB;
    }
    return values[index % count];
}

int32_t radio_mode_next_user_value(int32_t current, int delta) {
    uint8_t count = 0;
    const int32_t* values = radioModeUserValues(&count);
    if(count == 0) {
        return RADIO_MODE_WEB;
    }

    const int32_t sanitized = radio_mode_sanitize_value(current);
    int selected = 0;
    for(uint8_t i = 0; i < count; ++i) {
        if(values[i] == sanitized) {
            selected = i;
            break;
        }
    }

    const int direction = delta >= 0 ? 1 : -1;
    int steps = abs(delta);
    while(steps-- > 0) {
        selected += direction;
        if(selected < 0) selected = count - 1;
        if(selected >= count) selected = 0;
    }
    return values[selected];
}

bool radio_mode_is_web() {
    return currentMode == RADIO_MODE_WEB;
}

bool radio_mode_is_espnow() {
    return currentMode == RADIO_MODE_ESPNOW;
}

bool radio_mode_is_off() {
    return currentMode == RADIO_MODE_OFF;
}

bool radio_mode_is_dual() {
    return currentMode == RADIO_MODE_DUAL;
}

bool radio_mode_web_recovery_active() {
    return webRecoveryActive;
}

unsigned long radio_mode_recovery_remaining_ms() {
    if(!espnowRecoveryWatchActive) {
        return 0;
    }

    const unsigned long elapsed = millis() - espnowRecoveryWatchStartedAt;
    if(elapsed >= HEADLESS_ESPNOW_BOOT_RECOVERY_MS) {
        return 0;
    }
    return HEADLESS_ESPNOW_BOOT_RECOVERY_MS - elapsed;
}

const char* radio_mode_label(RadioMode mode) {
    switch(mode) {
        case RADIO_MODE_ESPNOW: return "ESP-NOW";
        case RADIO_MODE_OFF: return "Radio Off";
        case RADIO_MODE_DUAL: return "WiFi";
        case RADIO_MODE_WEB:
        default: return "WiFi";
    }
}

const char* radio_mode_value_label(int32_t value) {
    switch(radio_mode_from_value(radio_mode_sanitize_value(value))) {
        case RADIO_MODE_ESPNOW: return "ESP-NOW";
        case RADIO_MODE_OFF: return "Radio Off";
        case RADIO_MODE_DUAL:
        case RADIO_MODE_WEB:
        default: return "WiFi";
    }
}

bool radio_mode_status_ok() {
    if(radio_mode_is_off()) {
        return true;
    }
    if(radio_mode_is_espnow()) {
        return espnow_control_is_active();
    }
    if(radio_mode_is_dual()) {
        return wifi_ui_is_active();
    }
    return wifi_ui_is_active();
}

String radio_mode_lcd_status() {
    if(radio_mode_is_off()) {
        return "Radio off";
    }
    if(radio_mode_is_espnow()) {
        return "Remote link";
    }
    if(radio_mode_is_dual()) {
        return wifi_ui_lcd_status();
    }
    if(radio_mode_web_recovery_active()) {
        return "Web recovery";
    }
    if(internetSyncActive) {
        return "Internet sync";
    }
    return wifi_ui_lcd_status();
}

String radio_mode_status_text() {
    if(radio_mode_is_off()) {
        return "Radio off";
    }
    if(radio_mode_is_espnow()) {
        return "Wi-Fi disabled; remote link parked";
    }
    if(radio_mode_is_dual()) {
        return wifi_ui_is_active() ? wifi_ui_lcd_status() : String("Web off");
    }
    if(radio_mode_web_recovery_active()) {
        return String("Web recovery; saved ESP-NOW");
    }
    if(internetSyncActive) {
        return String("Internet sync; saved ESP-NOW");
    }
    return wifi_ui_is_active() ? wifi_ui_lcd_status() : String("Web off");
}
