#include "diag_console.h"

#include <cstdlib>
#include <cstring>

#include "display.h"
#include "error_history.h"
#include "gbw.h"
#include "main.h"
#include "ble_console.h"
#include "comm.h"
#include "motor_hal.h"
#include "pdata.h"

namespace {
bool streamEnabled = false;
unsigned long lastStreamAt = 0;
uint8_t lastObservedError = 0xFF;
uint16_t lastObservedFaultCode = 0xFFFF;
uint8_t lastObservedCommStatus = 0xFF;
uint8_t lastObservedMotorStatus = 0xFF;
uint8_t lastObservedScaleStatus = 0xFF;
char serialLineBuffer[96];
size_t serialLineLen = 0;

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
        case CALIBRATING: return "CALIBRATING";
        case MOTOR_SELECT: return "MOTOR_SELECT";
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
        case CALIBRATING: return "CAL";
        case MOTOR_SELECT: return "DRVSEL";
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
        default: return nullptr;
    }
}

uint8_t menuCountByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return NUM_MENU1_ITEMS;
        case 2: return NUM_MENU2_ITEMS;
        case 3: return NUM_MENU3_ITEMS;
        case 4: return NUM_MENU4_ITEMS;
        default: return 0;
    }
}

const char* menuLabel(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return "Main";
        case 2: return "Purge Settings";
        case 3: return "GbW Settings";
        case 4: return "JMC Baud";
        default: return "Unknown";
    }
}

States menuStateByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return MENU1;
        case 2: return MENU2;
        case 3: return MENU3;
        case 4: return MENU4;
        default: return IDLE;
    }
}

int8_t* menuSelectionByNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return &menu1Selected;
        case 2: return &menu2Selected;
        case 3: return &menu3Selected;
        case 4: return &menu4Selected;
        default: return nullptr;
    }
}

uint8_t currentMenuNumber() {
    switch(state) {
        case MENU1: return 1;
        case MENU2: return 2;
        case MENU3: return 3;
        case MENU4: return 4;
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

const char* apiMenuItemKind(uint8_t menuNum, uint8_t itemIdx) {
    if(menuNum == 1) {
        switch(itemIdx) {
            case Menu1Items::EXITMENU: return "nav";
            case Menu1Items::PURGESETTINGS:
            case Menu1Items::GBWSETTINGS:
            case Menu1Items::JMCBAUDMENU:
                return "submenu";
            case Menu1Items::CALIBRATE:
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

    return "unknown";
}

const char* apiMenuItemAction(uint8_t menuNum, uint8_t itemIdx) {
    if(menuNum == 1) {
        switch(itemIdx) {
            case Menu1Items::CALIBRATE: return "calibrate";
            case Menu1Items::RESET: return "reset_defaults";
            default: return nullptr;
        }
    }

    if(menuNum == 4 && itemIdx >= Menu4Items::JMCBAUD9600 && itemIdx <= Menu4Items::JMCBAUD115200) {
        return "jmc_baud";
    }

    return nullptr;
}

uint8_t apiTargetMenu(uint8_t menuNum, uint8_t itemIdx) {
    if(menuNum == 1) {
        switch(itemIdx) {
            case Menu1Items::PURGESETTINGS: return 2;
            case Menu1Items::GBWSETTINGS: return 3;
            case Menu1Items::JMCBAUDMENU: return 4;
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
    Serial.print(",\"error\":");
    Serial.print(static_cast<unsigned>(error));
    Serial.print(",\"errorText\":");
    jsonPrintQuoted(errorText(error));
    Serial.print(",\"setRPM\":");
    Serial.print(setRPM);
    Serial.print(",\"motorSetRPM\":");
    Serial.print(motor_setRPM);
    Serial.print(",\"currentRPM\":");
    Serial.print(motor_currentRPM);
    Serial.print(",\"torque\":");
    Serial.print(motor_currentTorque);
    Serial.print(",\"torquePercent\":");
    Serial.print(motor_get_torque_percent());
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
    const menuEntry* menu = menuByNumber(menuNum);
    if(menu == nullptr) {
        return;
    }

    const uint8_t count = menuCountByNumber(menuNum);
    const int8_t* selectedPtr = menuSelectionByNumber(menuNum);
    const uint8_t activeMenu = currentMenuNumber();

    Serial.print("{\"type\":\"menu\",\"menu\":");
    Serial.print(static_cast<unsigned>(menuNum));
    Serial.print(",\"label\":");
    jsonPrintQuoted(menuLabel(menuNum));
    Serial.print(",\"count\":");
    Serial.print(static_cast<unsigned>(count));
    Serial.print(",\"active\":");
    Serial.print(activeMenu == menuNum ? "true" : "false");
    Serial.print(",\"selected\":");
    Serial.print(selectedPtr != nullptr ? static_cast<int>(*selectedPtr) : -1);
    Serial.println("}");

    for(uint8_t idx = 0; idx < count; ++idx) {
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

    if(!apiMenuItemWritable(menuNum, itemIdx)) {
        *message = "item is not writable";
        return false;
    }

    if(menuNum == 1) {
        Menu1[itemIdx].value = clampMenuValue(Menu1[itemIdx], requestedValue);
        if(itemIdx == Menu1Items::SETMAX && Menu1[SETMAX].value < Menu1[SETMIN].value) {
            Menu1[SETMAX].value = Menu1[SETMIN].value;
        }
        if(itemIdx == Menu1Items::SETMIN && Menu1[SETMIN].value > Menu1[SETMAX].value) {
            Menu1[SETMIN].value = Menu1[SETMAX].value;
        }
        persistMenuValue(menuNum, itemIdx);
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
    } else {
        *message = "menu does not accept direct value writes";
        return false;
    }

    lastActivity = millis();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiSetRpmValue(int32_t requestedValue, const char** message) {
    setRPM = requestedValue;
    if(setRPM > Menu1[SETMAX].value) setRPM = Menu1[SETMAX].value;
    if(setRPM < Menu1[SETMIN].value) setRPM = Menu1[SETMIN].value;
    persistSetValue(IDLE);
    lastActivity = millis();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiSetWeightValue(int32_t requestedValue, const char** message) {
    if(requestedValue < static_cast<int32_t>(default_min_weight)) requestedValue = default_min_weight;
    if(requestedValue > static_cast<int32_t>(default_max_weight)) requestedValue = default_max_weight;
    setWeight = static_cast<uint32_t>(requestedValue);
    persistSetValue(IDLE_GBW);
    lastActivity = millis();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiActionStart(const char** message) {
    if(shouldOpenDriveSelectorApi()) {
        *message = "drive selection required";
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

    lastActivity = millis();
    disp_updateRequired = true;
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

    lastActivity = millis();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiActionIdle(States nextState, const char** message) {
    enterMenu = false;
    menu1Selected = EXITMENU;
    menu2Selected = RETURN_FROM_PURGE;
    menu3Selected = RETURN_FROM_GBW;
    menu4Selected = RETURN_FROM_JMC_BAUD;
    motor_setRPM = 0;
    state = nextState;
    lastActivity = millis();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiActionDrive(const char* mode, const char** message) {
    if(mode == nullptr) {
        *message = "missing drive mode";
        return false;
    }

    if(strcmp(mode, "auto") == 0) {
        motor_redetect_drive(false);
    } else if(strcmp(mode, "jmc") == 0) {
        motor_force_drive_type(DRIVE_TYPE_JMC, false);
    } else if(strcmp(mode, "rt") == 0) {
        motor_force_drive_type(DRIVE_TYPE_RT, false);
    } else {
        *message = "invalid drive mode";
        return false;
    }

    error = 0;
    errorCode = 0;
    commCounter = 0;
    lastActivity = millis();
    disp_updateRequired = true;
    *message = "ok";
    return true;
}

bool apiActionCalibrate(const char** message) {
    if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
        *message = "drive not detected";
        return false;
    }

    state = CALIBRATING;
    enterMenu = false;
    disp_updateRequired = true;
    lastActivity = millis();
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
    out += ",\"error\":";
    out += String(static_cast<unsigned>(error));
    out += ",\"errorText\":";
    appendJsonQuoted(out, errorText(error));
    out += ",\"setRPM\":";
    out += String(setRPM);
    out += ",\"motorSetRPM\":";
    out += String(motor_setRPM);
    out += ",\"currentRPM\":";
    out += String(motor_currentRPM);
    out += ",\"torque\":";
    out += String(motor_currentTorque);
    out += ",\"torquePercent\":";
    out += String(motor_get_torque_percent());
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

    for(uint8_t idx = 0; idx < count; ++idx) {
        if(idx > 0) out += ',';
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
    Serial.println("[diag] commands: ?=status, h=help, v=toggle stream, g=force IDLE_GBW, i=force IDLE, a=redetect, j=force JMC, r=force RT, p=probe, l=dump log");
    Serial.println("[diag] jmc: jbaud <9600|19200|38400|57600|115200> (write drive profile and store same baud for next boot; reboot required)");
    Serial.println("[diag] config: jb <9600|19200|38400|57600|115200> (set preferred JMC boot baud, takes effect on redetect/reboot)");
    Serial.println("[diag] raw: mr <j|r> <slave> <reg> <count> | mw <j|r> <slave> <reg> <value>");
    Serial.println("[diag] raw-uart: mru <baud> <8N1|8N2|8E1|8E2|8O1|8O2> <slave> <reg> <count> | mwu <baud> <mode> <slave> <reg> <value>");
    Serial.println("[diag] api: api snapshot | api set <menu> <item> <value> | api setrpm <rpm> | api setweight <mg>");
    Serial.println("[diag] api: api action <start|stop|idle|idle_gbw|redetect|calibrate|drive auto|drive jmc|drive rt|jmc_baud <baud>|reset_defaults confirm>");
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
        "[diag] drive=%s uart=%s jmc_pref=%lu state=%s comm=%s motor=%s err=%u setRPM=%d curRPM=%d torque=%d setWeight=%lu weight=%ld scale=%s saved_name=\"%s\" saved_mac=\"%s\" seen_name=\"%s\" seen_mac=\"%s\"\n",
        motor_get_type_label(),
        comm_get_profile_label(),
        static_cast<unsigned long>(motor_get_preferred_jmc_baud()),
        stateText(state),
        commText(commStatus),
        motorText(currentStatus),
        static_cast<unsigned>(error),
        motor_setRPM,
        motor_currentRPM,
        motor_currentTorque,
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
    motor_setRPM = 0;
    state = nextState;
    lastActivity = millis();
    disp_updateRequired = true;
}

void printProbeReport() {
    const MotorProbeReport report = motor_run_probe_report();
    Serial.printf(
        "[diag] probe jmc_6041=%s jmc_1010=%s jmc_6040w=%s jmc_baud=%lu rt_1300=%s active_drive=%s active_uart=%s\n",
        report.jmc_status_word ? "ok" : "no",
        report.jmc_magic_word ? "ok" : "no",
        report.jmc_control_word ? "ok" : "no",
        static_cast<unsigned long>(report.jmc_baud),
        report.rt_status_block ? "ok" : "no",
        motor_get_type_label(),
        comm_get_profile_label()
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
                snprintf(line,
                         sizeof(line),
                         "#%lu t=%lums %s PROBE j6041=%u j1010=%u j6040w=%u jbaud=%s rt1300=%u pick=%s st=%s",
                         static_cast<unsigned long>(entry.sequence),
                         static_cast<unsigned long>(entry.uptime_ms),
                         levelText(entry.level),
                         static_cast<unsigned>((entry.value >> 0) & 0x1U),
                         static_cast<unsigned>((entry.value >> 1) & 0x1U),
                         static_cast<unsigned>((entry.value >> 2) & 0x1U),
                         jmcBaudLabel,
                         static_cast<unsigned>((entry.value >> 3) & 0x1U),
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

    if(strcmp(token, "j") == 0 || strcmp(token, "J") == 0 || strcmp(token, "jmc") == 0 || strcmp(token, "JMC") == 0) {
        *profile = COMM_PROFILE_JMC;
        return true;
    }

    if(strcmp(token, "r") == 0 || strcmp(token, "R") == 0 || strcmp(token, "rt") == 0 || strcmp(token, "RT") == 0) {
        *profile = COMM_PROFILE_RT;
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

        if(strcmp(subcmd, "status") == 0) {
            apiPrintStatus();
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
                ok = motor_run_jmc_baud_switch(baud);
                message = motor_get_jmc_baud_trial_status_text();
                disp_updateRequired = true;
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
            motor_redetect_drive(false);
            Serial.println("[diag] redetect complete");
            printStatus();
            break;
        case 'j':
        case 'J':
            motor_force_drive_type(DRIVE_TYPE_JMC, false);
            Serial.println("[diag] forced drive=JMC");
            printStatus();
            break;
        case 'r':
        case 'R':
            motor_force_drive_type(DRIVE_TYPE_RT, false);
            Serial.println("[diag] forced drive=RT");
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
    for(uint8_t menuNum = 1; menuNum <= 4; ++menuNum) {
        if(menuNum > 1) out += ',';
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
        } else {
            const uint32_t baud = static_cast<uint32_t>(strtoul(arg0, nullptr, 10));
            ok = motor_run_jmc_baud_switch(baud);
            rawMessage = motor_get_jmc_baud_trial_status_text();
            disp_updateRequired = true;
        }
    } else if(strcmp(action, "reset_defaults") == 0) {
        ok = apiActionResetDefaults(arg0, &rawMessage);
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
        "[diag] build=%s built=%s %s drive=%s uart=%s build_id=%s\n",
        __FILE__,
        __DATE__,
        __TIME__,
        motor_get_type_label(),
        comm_get_profile_label(),
        BUILD_ID
    );
    printHelp();
    printStatus();
}

void diag_console_poll() {
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
