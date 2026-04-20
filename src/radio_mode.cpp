#include "radio_mode.h"

#include "display.h"
#include "espnow_control.h"
#include "main.h"
#include "wifi_ui.h"

namespace {
RadioMode currentMode = RADIO_MODE_WEB;
bool initialized = false;

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
}

void radio_mode_init() {
    wifi_ui_init();
    initialized = false;
    radio_mode_apply(true);
}

void radio_mode_poll() {
    if(radio_mode_is_web()) {
        wifi_ui_poll();
    } else {
        espnow_control_poll();
    }
}

bool radio_mode_apply(bool force) {
    const RadioMode desiredMode = radio_mode_desired();
    const RadioMode previousMode = currentMode;
    if(initialized && !force && desiredMode == currentMode) {
        return true;
    }

    if(initialized || force) {
        wifi_ui_stop();
        espnow_control_stop();
    }

    bool ok = false;
    if(desiredMode == RADIO_MODE_WEB) {
        ok = wifi_ui_start();
    } else {
        ok = espnow_control_start();
    }

    if(ok) {
        currentMode = desiredMode;
        initialized = true;
        disp_updateRequired = true;
        Serial.printf("[radio] mode=%s\n", radio_mode_label(currentMode));
        return true;
    }

    if(desiredMode != RADIO_MODE_WEB) {
        if(wifi_ui_start()) {
            currentMode = RADIO_MODE_WEB;
            initialized = true;
            disp_updateRequired = true;
            Serial.println("[radio] esp-now start failed, restored WEB mode");
        }
    } else if(previousMode == RADIO_MODE_ESPNOW && espnow_control_start()) {
        currentMode = RADIO_MODE_ESPNOW;
        initialized = true;
        disp_updateRequired = true;
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
    return Menu1[Menu1Items::RADIOMODE].value == RADIO_MODE_ESPNOW ? RADIO_MODE_ESPNOW : RADIO_MODE_WEB;
}

bool radio_mode_is_web() {
    return currentMode == RADIO_MODE_WEB;
}

bool radio_mode_is_espnow() {
    return currentMode == RADIO_MODE_ESPNOW;
}

const char* radio_mode_label(RadioMode mode) {
    return mode == RADIO_MODE_ESPNOW ? "ESP-NOW" : "WEB";
}

bool radio_mode_status_ok() {
    if(radio_mode_is_espnow()) {
        return espnow_control_is_active();
    }
    return wifi_ui_sta_connected();
}

String radio_mode_lcd_status() {
    if(radio_mode_is_espnow()) {
        return String("ESP-NOW ch") + String(static_cast<unsigned>(espnow_control_channel()));
    }
    return wifi_ui_lcd_status();
}

String radio_mode_status_text() {
    if(radio_mode_is_espnow()) {
        return espnow_control_status_text();
    }
    return wifi_ui_is_active() ? wifi_ui_lcd_status() : String("Web off");
}
