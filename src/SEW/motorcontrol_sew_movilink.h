#pragma once

#include "motor_backend.h"

struct MotorProbeReport;

bool motor_sew_movilink_probe_backend(MotorProbeReport* report);
void motor_sew_movilink_init_backend();
bool motor_sew_movilink_do_comm_backend();
bool motor_sew_movilink_off_backend();
bool motor_sew_movilink_calibrate_backend();
void motor_sew_movilink_sleep_backend();
void motor_sew_movilink_reset_backend();

extern const MotorBackendDefinition MOTOR_BACKEND_SEW_MOVILINK;
