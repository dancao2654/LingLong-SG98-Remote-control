#pragma once
#include "motor_backend.h"

enum COMM_STATUS { COMM_DISCONNECTED, COMM_CONNECTED };
enum SYSTEM_STATUS { MOTOR_NOT_CONNECTED, MOTOR_NOT_READY, MOTOR_READY, MOTOR_ENABLED, MOTOR_FAULT };
enum MotorCommandId { MOTOR_COMMAND_TORQUE_LIMIT };
struct MotorProbeReport { bool tse_signature; };
extern int16_t motor_setRPM;
extern int16_t motor_currentRPM;
extern int16_t motor_currentTorque;
extern int16_t commCounter;
extern COMM_STATUS commStatus;
extern SYSTEM_STATUS currentStatus;
int16_t motor_get_effective_set_rpm();
int16_t motor_get_formatted_command_value(MotorCommandId command, int8_t sign);
