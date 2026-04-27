#pragma once

#include "definitions.h"
#include "comm.h"
#include "motor_backend.h"

enum COMM_STATUS { COMM_DISCONNECTED, COMM_CONNECTED, COMM_INVALID_STATUS };
enum SYSTEM_STATUS { MOTOR_NOT_CONNECTED, MOTOR_NOT_READY, MOTOR_READY, MOTOR_ENABLED, MOTOR_FAULT, MOTOR_INVALID };
enum DriveType { DRIVE_TYPE_UNKNOWN, DRIVE_TYPE_JMC, DRIVE_TYPE_RT, DRIVE_TYPE_KRS_130ST_M07725 };
enum MotorCommandId {
    MOTOR_COMMAND_SET_RPM,
    MOTOR_COMMAND_TORQUE_LIMIT,
    MOTOR_COMMAND_RAMP_LEVEL,
    MOTOR_COMMAND_DIRECTION,
};
enum JmcBaudTrialStatus {
    JMC_BAUD_SWITCH_IDLE,
    JMC_BAUD_SWITCH_RUNNING,
    JMC_BAUD_SWITCH_SET_OK,
    JMC_BAUD_SWITCH_ALREADY_SET,
    JMC_BAUD_SWITCH_FAIL_NO_JMC,
    JMC_BAUD_SWITCH_FAIL_WRITE,
    JMC_BAUD_SWITCH_FAIL_VERIFY,
    JMC_BAUD_SWITCH_FAIL_LOST,
};

struct MotorProbeReport {
    bool jmc_status_word;
    bool jmc_magic_word;
    bool jmc_control_word;
    bool rt_status_block;
    bool krs_130st_m07725_motor_code;
    uint32_t jmc_baud;
};

struct MotorCommandDescriptor {
    MotorCommandId id;
    uint8_t menuNum;
    uint8_t itemIdx;
    int16_t valueScale;
    bool supported;
    const char* label;
    const char* unit;
    const char* helpText;
    const char* guidance;
};

struct MotorDescriptor {
    DriveType driveType;
    const char* key;
    const char* label;
    const char* description;
    const char* guidance;
    CommProfile commProfile;
    int16_t maxRpm;
    int16_t torquePercentNumerator;
    uint8_t probePriority;
    const MotorCommandDescriptor* commands;
    uint8_t commandCount;
    const MotorBackendDefinition* backend;
};

extern int16_t motor_setRPM;
extern int16_t motor_currentRPM;
extern int16_t motor_currentTorque;
extern int16_t calibrateArray[];
extern uint16_t currentCal;
extern int16_t commCounter;
extern COMM_STATUS commStatus;
extern SYSTEM_STATUS currentStatus;
extern DriveType motorDriveType;

void motor_init();
bool do_comm();
bool motorOff();
bool Calibrate();
void motorSleep();
void motorReset();

const MotorDescriptor* motor_get_descriptor(DriveType driveType);
const MotorDescriptor* motor_get_current_descriptor();
const MotorBackendDefinition* motor_get_backend_definition(DriveType driveType);
const MotorBackendDefinition* motor_get_current_backend_definition();
const MotorCommandDescriptor* motor_get_command_descriptor(DriveType driveType, MotorCommandId commandId);
const MotorCommandDescriptor* motor_get_current_command_descriptor(MotorCommandId commandId);
const MotorCommandDescriptor* motor_get_command_descriptor_for_menu(DriveType driveType, uint8_t menuNum, uint8_t itemIdx);
const MotorCommandDescriptor* motor_get_current_command_descriptor_for_menu(uint8_t menuNum, uint8_t itemIdx);
DriveType motor_get_drive_type();
const char* motor_get_type_key();
const char* motor_get_type_label();
const char* motor_get_type_description();
const char* motor_get_type_guidance();
const char* motor_get_backend_family_label();
const char* motor_get_backend_probe_guidance();
const char* motor_get_backend_command_guidance();
const char* motor_get_backend_telemetry_guidance();
int16_t motor_get_max_rpm();
bool motor_supports_ramp();
bool motor_jmc_tools_supported();
bool motor_menu_item_visible(uint8_t menuNum, uint8_t itemIdx);
bool motor_is_direction_reversed();
int8_t motor_get_direction_multiplier();
int16_t motor_apply_direction(int16_t rpm);
int16_t motor_get_effective_set_rpm();
int16_t motor_get_torque_percent();
bool motor_command_supported(MotorCommandId commandId);
int16_t motor_get_command_menu_value(MotorCommandId commandId);
int16_t motor_format_command_value(MotorCommandId commandId, int16_t uiValue, int8_t sign = 1);
int16_t motor_get_formatted_command_value(MotorCommandId commandId, int8_t sign = 1);
void motor_force_drive_type(DriveType driveType, bool persist = false);
void motor_redetect_drive(bool persist = false);
MotorProbeReport motor_run_probe_report();
uint32_t motor_get_preferred_jmc_baud();
bool motor_set_preferred_jmc_baud(uint32_t baud_rate, bool persist = true);
bool motor_run_jmc_baud_switch(uint32_t baud_rate);
JmcBaudTrialStatus motor_get_jmc_baud_trial_status();
uint32_t motor_get_jmc_baud_trial_result_baud();
uint32_t motor_get_jmc_baud_trial_read_before();
uint32_t motor_get_jmc_baud_trial_read_after();
const char* motor_get_jmc_baud_trial_status_text();
