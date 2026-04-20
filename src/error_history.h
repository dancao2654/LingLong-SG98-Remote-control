#ifndef ERROR_HISTORY_H
#define ERROR_HISTORY_H

#include <Arduino.h>

struct ErrorHistoryEntry {
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t value;
    uint16_t code;
    uint8_t source;
    uint8_t level;
    uint8_t state;
};

void error_history_init();

enum ErrorHistorySource : uint8_t {
    ERROR_HISTORY_SOURCE_SYSTEM = 1,
    ERROR_HISTORY_SOURCE_MOTOR = 2,
    ERROR_HISTORY_SOURCE_SCALE = 3,
};

enum ErrorHistoryLevel : uint8_t {
    ERROR_HISTORY_LEVEL_INFO = 1,
    ERROR_HISTORY_LEVEL_WARN = 2,
    ERROR_HISTORY_LEVEL_ERROR = 3,
};

enum ErrorHistoryCode : uint16_t {
    ERROR_HISTORY_CODE_SYSTEM_ERROR = 1,
    ERROR_HISTORY_CODE_MOTOR_COMM_STATUS = 100,
    ERROR_HISTORY_CODE_MOTOR_STATUS = 101,
    ERROR_HISTORY_CODE_MOTOR_PROBE = 102,
    ERROR_HISTORY_CODE_MOTOR_RPM_TEST = 103,
    ERROR_HISTORY_CODE_MOTOR_JMC_DIAG = 104,
    ERROR_HISTORY_CODE_MOTOR_JMC_BAUD = 105,
    ERROR_HISTORY_CODE_MOTOR_JMC_BAUD_TRIAL = 106,
    ERROR_HISTORY_CODE_SCALE_STATUS = 200,
};

enum ErrorHistoryMotorRpmSubtype : uint8_t {
    ERROR_HISTORY_MOTOR_RPM_SAMPLE = 1,
    ERROR_HISTORY_MOTOR_RPM_ZERO = 2,
};

enum ErrorHistoryMotorRpmSource : uint8_t {
    ERROR_HISTORY_MOTOR_RPM_SRC_JMC_2114 = 1,
    ERROR_HISTORY_MOTOR_RPM_SRC_JMC_606C = 2,
    ERROR_HISTORY_MOTOR_RPM_SRC_RT_1301 = 3,
};

enum ErrorHistoryMotorDiagSubtype : uint8_t {
    ERROR_HISTORY_MOTOR_DIAG_MODE_RESPONSE = 1,
    ERROR_HISTORY_MOTOR_DIAG_DRIVE_ERROR = 2,
};

enum ErrorHistoryMotorJmcBaudSubtype : uint8_t {
    ERROR_HISTORY_MOTOR_JMC_BAUD_DETECTED = 1,
    ERROR_HISTORY_MOTOR_JMC_BAUD_PREFERRED = 2,
    ERROR_HISTORY_MOTOR_JMC_BAUD_READ_BEFORE = 3,
    ERROR_HISTORY_MOTOR_JMC_BAUD_READ_AFTER = 4,
};

enum ErrorHistoryMotorJmcBaudTrialSubtype : uint8_t {
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_STARTED = 1,
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_OK = 2,
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_ALREADY = 3,
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_NO_JMC = 4,
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_WRITE_FAIL = 6,
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_VERIFY_FAIL = 7,
    ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_LOST = 8,
};

void error_history_log(uint8_t source,
                       uint8_t level,
                       uint16_t code,
                       uint32_t value,
                       uint8_t state);
uint16_t error_history_count();
bool error_history_get(uint16_t index_from_oldest, ErrorHistoryEntry* entry);

#endif
