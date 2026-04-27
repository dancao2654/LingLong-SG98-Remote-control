#include "radio_mode.h"

#include "controller_board.h"
#include "display.h"
#include "espnow_control.h"
#include "main.h"
#include "wifi_ui.h"

namespace {
constexpr unsigned long HEADLESS_ESPNOW_BOOT_RECOVERY_MS = 60000;

RadioMode currentMode = RADIO_MODE_WEB;
bool initialized = false;
bool espnowRecoveryWatchActive = false;
bool webRecoveryActive = false;
unsigned long espnowRecoveryWatchStartedAt = 0;

bool isMenuOrIdleState(States currentState) {
    switch(currentState) {
        case IDLE:
        case IDLE_GBW:
        case MENU1:
        case MENU2:
        case MENU3:
        case MENU4:
        case MOTOR_SELECT:
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
    if(!espnowRecoveryWatchActive || espnow_control_last_command_rx_at() == 0) {
        return;
    }

    clearEspNowRecoveryWatch();
    Serial.println("[radio] headless ESP-NOW remote seen; staying on ESP-NOW");
}

void switchToWebRecoverySession() {
    if(!espnowRecoveryWatchActive || currentMode != RADIO_MODE_ESPNOW) {
        return;
    }

    clearEspNowRecoveryWatch();
    espnow_control_stop();
    const bool webOk = wifi_ui_start();
    const bool espOk = webOk && espnow_control_start(true);
    if(webOk) {
        currentMode = espOk ? RADIO_MODE_DUAL : RADIO_MODE_WEB;
        webRecoveryActive = true;
        disp_updateRequired = true;
        Serial.println(espOk
                           ? "[radio] headless ESP-NOW timeout; WEB+ESP recovery active for this boot"
                           : "[radio] headless ESP-NOW timeout; WEB recovery active for this boot");
        return;
    }

    if(espnow_control_start()) {
        currentMode = RADIO_MODE_ESPNOW;
        startEspNowRecoveryWatchIfNeeded();
        Serial.println("[radio] WEB recovery failed; restored ESP-NOW");
    }
}

void serviceEspNowRecoveryWatch() {
    if(!espnowRecoveryWatchActive) {
        return;
    }

    noteEspNowRecoverySatisfiedIfNeeded();
    if(!espnowRecoveryWatchActive) {
        return;
    }

    const unsigned long now = millis();
    if(now - espnowRecoveryWatchStartedAt >= HEADLESS_ESPNOW_BOOT_RECOVERY_MS) {
        switchToWebRecoverySession();
    }
}
}

void radio_mode_init() {
    wifi_ui_init();
    initialized = false;
    radio_mode_apply(true);
}

void radio_mode_poll() {
    if(radio_mode_is_web() || radio_mode_is_dual()) {
        wifi_ui_poll();
    }
    if(radio_mode_is_espnow() || radio_mode_is_dual()) {
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
        }
        return true;
    }

    if(initialized || force) {
        clearEspNowRecoveryWatch();
        webRecoveryActive = false;
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
        currentMode = desiredMode;
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
    if(controller_board_requires_remote_recovery() && mode == RADIO_MODE_OFF) {
        return RADIO_MODE_DUAL;
    }
    return mode;
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
        case RADIO_MODE_OFF: return "OFF";
        case RADIO_MODE_DUAL: return "WEB+ESP";
        case RADIO_MODE_WEB:
        default: return "WEB";
    }
}

const char* radio_mode_value_label(int32_t value) {
    switch(radio_mode_from_value(value)) {
        case RADIO_MODE_ESPNOW: return "ESP-NOW";
        case RADIO_MODE_OFF: return "OFF";
        case RADIO_MODE_DUAL: return "Web + ESP";
        case RADIO_MODE_WEB:
        default: return "Web UI";
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
        return wifi_ui_is_active() && espnow_control_is_active();
    }
    return wifi_ui_sta_connected();
}

String radio_mode_lcd_status() {
    if(radio_mode_is_off()) {
        return "Radio off";
    }
    if(radio_mode_is_espnow()) {
        return String("ESP-NOW ch") + String(static_cast<unsigned>(espnow_control_channel()));
    }
    if(radio_mode_is_dual()) {
        return String("Web+ESP ch") + String(static_cast<unsigned>(espnow_control_channel()));
    }
    if(radio_mode_web_recovery_active()) {
        return "Web recovery";
    }
    return wifi_ui_lcd_status();
}

String radio_mode_status_text() {
    if(radio_mode_is_off()) {
        return "Radio off";
    }
    if(radio_mode_is_espnow()) {
        return espnow_control_status_text();
    }
    if(radio_mode_is_dual()) {
        String out = wifi_ui_is_active() ? wifi_ui_lcd_status() : String("Web off");
        out += " + ";
        out += espnow_control_status_text();
        return out;
    }
    if(radio_mode_web_recovery_active()) {
        return String("Web recovery; saved ESP-NOW");
    }
    return wifi_ui_is_active() ? wifi_ui_lcd_status() : String("Web off");
}
