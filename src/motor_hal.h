#pragma once

#include "definitions.h"

enum COMM_STATUS { COMM_DISCONNECTED, COMM_CONNECTED, COMM_INVALID_STATUS };
enum SYSTEM_STATUS { MOTOR_NOT_CONNECTED, MOTOR_NOT_READY, MOTOR_READY, MOTOR_ENABLED, MOTOR_FAULT, MOTOR_INVALID };
enum DriveType { DRIVE_TYPE_UNKNOWN, DRIVE_TYPE_JMC, DRIVE_TYPE_RT };
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
    uint32_t jmc_baud;
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

DriveType motor_get_drive_type();
const char* motor_get_type_label();
int16_t motor_get_max_rpm();
bool motor_supports_ramp();
bool motor_is_direction_reversed();
int8_t motor_get_direction_multiplier();
int16_t motor_apply_direction(int16_t rpm);
int16_t motor_get_effective_set_rpm();
int16_t motor_get_torque_percent();
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
