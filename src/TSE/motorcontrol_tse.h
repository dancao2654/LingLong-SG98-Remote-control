#pragma once

#include "motor_backend.h"

extern const MotorBackendDefinition MOTOR_BACKEND_TSE;
bool motor_tse_is_kfm();
bool motor_tse_stationary_ready_for_baud_change();
