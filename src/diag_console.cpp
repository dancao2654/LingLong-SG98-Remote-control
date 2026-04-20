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

void printHelp() {
    Serial.println("[diag] commands: ?=status, h=help, v=toggle stream, g=force IDLE_GBW, i=force IDLE, a=redetect, j=force JMC, r=force RT, p=probe, l=dump log");
    Serial.println("[diag] jmc: jbaud <9600|19200|38400|57600|115200> (write drive profile and store same baud for next boot; reboot required)");
    Serial.println("[diag] config: jb <9600|19200|38400|57600|115200> (set preferred JMC boot baud, takes effect on redetect/reboot)");
    Serial.println("[diag] raw: mr <j|r> <slave> <reg> <count> | mw <j|r> <slave> <reg> <value>");
    Serial.println("[diag] raw-uart: mru <baud> <8N1|8N2|8E1|8E2|8O1|8O2> <slave> <reg> <count> | mwu <baud> <mode> <slave> <reg> <value>");
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
