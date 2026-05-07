#include "remote_link.h"

#include <cstring>

#include "burr_gap.h"
#include "controller_env.h"
#include "controller_board.h"
#include "controller_ota.h"
#include "comm.h"
#include "diag_console.h"
#include "display.h"
#include "gbw.h"
#include "main.h"
#include "motor_hal.h"
#include "wifi_ui.h"
#include <sg98_remote_protocol.h>

namespace {

unsigned long g_remoteLinkLastActivityAt = 0;
unsigned long g_remoteLinkRunningObservedAt = 0;
States g_remoteLinkLastObservedState = IDLE;
bool g_remoteLinkDeadmanLatched = false;

void setMessage(String* message, const String& value) {
    if(message != nullptr) {
        *message = value;
    }
}

const char* stateTextCompact(States value) {
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
        default: return "UNKNOWN";
    }
}

bool remoteSafetyState(States value) {
    switch(value) {
        case GRINDING:
        case PURGING:
        case GRINDING_GBW:
            return true;
        default:
            return false;
    }
}

void stopForRemoteDeadman() {
    const States expiredState = state;
    const unsigned long age = g_remoteLinkLastActivityAt == 0
                                  ? 0
                                  : millis() - g_remoteLinkLastActivityAt;
    motor_setRPM = 0;
    if(expiredState == GRINDING_GBW) {
        state = IDLE_GBW;
    } else {
        purge_cancel_pending();
        state = IDLE;
    }
    disp_updateRequired = true;
    g_remoteLinkDeadmanLatched = true;
    Serial.printf("[remote_link] headless remote deadman expired state=%s age=%lu ms\n",
                  stateTextCompact(expiredState),
                  static_cast<unsigned long>(age));
}

bool finishCommand(bool ok) {
    if(ok) {
        remote_link_note_activity();
    }
    return ok;
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

const char* commTextCompact(COMM_STATUS value) {
    switch(value) {
        case COMM_DISCONNECTED: return "DISCONNECTED";
        case COMM_CONNECTED: return "CONNECTED";
        default: return "UNKNOWN";
    }
}

const char* motorTextCompact(SYSTEM_STATUS value) {
    switch(value) {
        case MOTOR_NOT_CONNECTED: return "NOT_CONNECTED";
        case MOTOR_NOT_READY: return "NOT_READY";
        case MOTOR_READY: return "READY";
        case MOTOR_ENABLED: return "ENABLED";
        case MOTOR_FAULT: return "FAULT";
        default: return "UNKNOWN";
    }
}

const char* scaleTextCompact(SCALE_STATUS value) {
    switch(value) {
        case SCALE_DISCONNECTED: return "DISCONNECTED";
        case SCALE_CONNECTED: return "CONNECTED";
        case SCALE_CONNECTING: return "CONNECTING";
        case SCALE_LOST: return "LOST";
        default: return "UNKNOWN";
    }
}

const menuEntry* menuByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1:
            syncGaugeMenuValue();
            return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        case 4: return Menu4;
        case 5:
            Menu5[Menu5Items::WIRELESS_RADIO_MODE].value = Menu1[Menu1Items::RADIOMODE].value;
            return Menu5;
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

bool parseIntArgument(const String& raw, const char* prefix, int32_t* value) {
    const String token = String(prefix) + "=";
    if(!raw.startsWith(token)) {
        return false;
    }

    String number = raw.substring(token.length());
    number.trim();
    if(number.length() == 0) {
        return false;
    }

    *value = number.toInt();
    return true;
}

bool parseSetMenuCommand(const String& raw, uint8_t* menuNum, uint8_t* itemIdx, int32_t* value) {
    if(!raw.startsWith("SET_MENU=")) {
        return false;
    }

    const String body = raw.substring(strlen("SET_MENU="));
    const int firstComma = body.indexOf(',');
    const int secondComma = body.indexOf(',', firstComma + 1);
    if(firstComma <= 0 || secondComma <= firstComma + 1) {
        return false;
    }

    *menuNum = static_cast<uint8_t>(body.substring(0, firstComma).toInt());
    *itemIdx = static_cast<uint8_t>(body.substring(firstComma + 1, secondComma).toInt());
    *value = body.substring(secondComma + 1).toInt();
    return true;
}

bool runAction(const char* action, const char* arg0, String* message) {
    return diag_api_run_action(action, arg0, nullptr, message);
}

bool handleActionCommand(const String& rawAction, String* message) {
    String actionName = rawAction;
    String arg0;
    const int comma = rawAction.indexOf(',');
    if(comma >= 0) {
        actionName = rawAction.substring(0, comma);
        arg0 = rawAction.substring(comma + 1);
        arg0.trim();
    }

    actionName.trim();
    actionName.toLowerCase();
    if(arg0.length() > 0) {
        arg0.toLowerCase();
    }

    if(actionName == "reset_network") {
        wifi_ui_reset_network_settings();
        setMessage(message, "ok");
        return true;
    }

    return runAction(actionName.c_str(), arg0.length() > 0 ? arg0.c_str() : nullptr, message);
}

}  // namespace

String remote_link_build_status_frame() {
    String out;
    out.reserve(224);
    out += sg98_protocol::kFrameStatus;
    out += sg98_protocol::kFrameSeparator;
    out += stateTextCompact(state);
    out += sg98_protocol::kFrameSeparator;
    out += motor_get_type_label();
    out += sg98_protocol::kFrameSeparator;
    out += commTextCompact(commStatus);
    out += sg98_protocol::kFrameSeparator;
    out += motorTextCompact(currentStatus);
    out += sg98_protocol::kFrameSeparator;
    out += scaleTextCompact(scaleStatus);
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<int>(error));
    out += sg98_protocol::kFrameSeparator;
    out += String(setRPM);
    out += sg98_protocol::kFrameSeparator;
    out += String(motor_get_effective_set_rpm());
    out += sg98_protocol::kFrameSeparator;
    out += String(motor_currentRPM);
    out += sg98_protocol::kFrameSeparator;
    out += String(motor_get_torque_percent());
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<long>(setWeight));
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<long>(currentWeight));
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<int>(currentMenuNumber()));
    out += sg98_protocol::kFrameSeparator;
    out += enterMenu ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<unsigned long>(comm_get_jmc_baud()));
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<unsigned long>(motor_get_preferred_jmc_baud()));
    out += sg98_protocol::kFrameSeparator;
    out += wifi_ui_setup_complete() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += wifi_ui_has_saved_network() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += wifi_ui_sta_connected() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<unsigned>(wifi_ui_ap_station_count()));
    out += sg98_protocol::kFrameSeparator;
    out += controller_env_time_valid() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += controller_env_weather_valid() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += burr_gap_enabled() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += burr_gap_has_value() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += burr_gap_live() ? '1' : '0';
    out += sg98_protocol::kFrameSeparator;
    out += String(static_cast<long>(burr_gap_display_lcd_um()));
    if(controller_ota_ready_for_remote()) {
        out += sg98_protocol::kFrameSeparator;
        out += '1';
    }
    return out;
}

String remote_link_build_menu_frame(uint8_t menuNum) {
    String out;
    const menuEntry* menu = menuByNumber(menuNum);
    const uint8_t count = menuCountByNumber(menuNum);
    if(menu == nullptr || count == 0) {
        return out;
    }

    out.reserve(176);
    out += sg98_protocol::kFrameMenu;
    out += sg98_protocol::kFrameSeparator;
    out += String(menuNum);
    out += sg98_protocol::kFrameSeparator;
    for(uint8_t idx = 0; idx < count; ++idx) {
        if(idx > 0) {
            out += sg98_protocol::kListSeparator;
        }
        out += String(menu[idx].value);
    }
    return out;
}

String remote_link_build_environment_frame() {
    return controller_env_build_frame();
}

bool remote_link_handle_command(const String& rawPayload, String* message, bool* sendStatus, int8_t* menuRefresh) {
    if(sendStatus == nullptr || menuRefresh == nullptr) {
        setMessage(message, "invalid args");
        return false;
    }

    *sendStatus = false;
    *menuRefresh = 0;

    String cmd = rawPayload;
    cmd.trim();
    if(cmd.length() == 0) {
        setMessage(message, "empty command");
        return false;
    }

    sg98_protocol::EnvironmentFrame environmentFrame;
    if(sg98_protocol::parseEnvironmentFrame(cmd, &environmentFrame)) {
        const bool applied = controller_env_apply_peer_frame(environmentFrame);
        setMessage(message, applied ? "env synced" : "env ok");
        remote_link_note_activity();
        return true;
    }

    sg98_protocol::WifiConfigFrame wifiFrame;
    if(sg98_protocol::parseWifiConfigFrame(cmd, &wifiFrame)) {
        const bool ok = wifi_ui_save_network_settings(wifiFrame.ssid, wifiFrame.password);
        *sendStatus = true;
        setMessage(message, ok ? "wifi saved" : "wifi not saved");
        return finishCommand(ok);
    }

    String cmdUpper = cmd;
    cmdUpper.toUpperCase();

    if(cmdUpper == sg98_protocol::kCmdGetStatus) {
        *sendStatus = true;
        setMessage(message, "ok");
        remote_link_note_activity();
        return true;
    }

    if(cmdUpper == sg98_protocol::kCmdHeartbeat) {
        setMessage(message, "ok");
        remote_link_note_activity();
        return true;
    }

    if(cmdUpper.startsWith("GET_MENU=")) {
        const uint8_t menuNum = static_cast<uint8_t>(cmd.substring(strlen("GET_MENU=")).toInt());
        if(menuByNumber(menuNum) == nullptr) {
            setMessage(message, "invalid menu");
            return false;
        }
        *menuRefresh = static_cast<int8_t>(menuNum);
        setMessage(message, "ok");
        remote_link_note_activity();
        return true;
    }

    uint8_t menuNum = 0;
    uint8_t itemIdx = 0;
    int32_t value = 0;
    if(parseSetMenuCommand(cmd, &menuNum, &itemIdx, &value)) {
        const bool ok = diag_api_set_menu_value(menuNum, itemIdx, value, message);
        *sendStatus = true;
        if(ok) {
            *menuRefresh = static_cast<int8_t>(menuNum);
        }
        return finishCommand(ok);
    }

    if(cmdUpper.startsWith("ACTION=")) {
        const bool ok = handleActionCommand(cmd.substring(strlen("ACTION=")), message);
        *sendStatus = true;
        return finishCommand(ok);
    }

    if(cmdUpper == "START" || cmdUpper == "BUTTON_START") {
        *sendStatus = true;
        return finishCommand(runAction("start", nullptr, message));
    }
    if(cmdUpper == "STOP" || cmdUpper == "BUTTON_STOP") {
        *sendStatus = true;
        return finishCommand(runAction("stop", nullptr, message));
    }
    if(cmdUpper == "IDLE" || cmdUpper == "MODE_IDLE" || cmdUpper == "MODE_NORMAL") {
        *sendStatus = true;
        return finishCommand(runAction("idle", nullptr, message));
    }
    if(cmdUpper == "IDLE_GBW" || cmdUpper == "MODE_GBW") {
        *sendStatus = true;
        return finishCommand(runAction("idle_gbw", nullptr, message));
    }

    if(parseIntArgument(cmdUpper, "SET_RPM", &value) || parseIntArgument(cmdUpper, "RPM", &value)) {
        *sendStatus = true;
        return finishCommand(diag_api_set_rpm(value, message));
    }
    if(parseIntArgument(cmdUpper, "STEP_RPM", &value)) {
        *sendStatus = true;
        return finishCommand(diag_api_set_rpm(static_cast<int32_t>(setRPM) + value, message));
    }
    if(parseIntArgument(cmdUpper, "SET_WEIGHT", &value) || parseIntArgument(cmdUpper, "WEIGHT", &value)) {
        *sendStatus = true;
        return finishCommand(diag_api_set_weight(value, message));
    }
    if(parseIntArgument(cmdUpper, "STEP_WEIGHT", &value)) {
        *sendStatus = true;
        return finishCommand(diag_api_set_weight(static_cast<int32_t>(setWeight) + value, message));
    }

    setMessage(message, "unknown remote command");
    return false;
}

void remote_link_note_activity() {
    g_remoteLinkLastActivityAt = millis();
    g_remoteLinkDeadmanLatched = false;
}

unsigned long remote_link_last_activity_at() {
    return g_remoteLinkLastActivityAt;
}

void remote_link_safety_poll() {
    const unsigned long now = millis();
    const bool safetyState = remoteSafetyState(state);
    if(state != g_remoteLinkLastObservedState) {
        g_remoteLinkLastObservedState = state;
        g_remoteLinkRunningObservedAt = safetyState ? now : 0;
        if(!safetyState) {
            g_remoteLinkDeadmanLatched = false;
        }
    }

    if(!safetyState || g_remoteLinkDeadmanLatched || !controller_board_requires_remote_recovery()) {
        return;
    }

    const unsigned long basis = g_remoteLinkLastActivityAt > g_remoteLinkRunningObservedAt
                                    ? g_remoteLinkLastActivityAt
                                    : g_remoteLinkRunningObservedAt;
    if(basis == 0 || now - basis <= HEADLESS_REMOTE_DEADMAN_MS) {
        return;
    }

    stopForRemoteDeadman();
}
