#pragma once

#include "motor_backend.h"

struct MotorProbeReport;

bool motor_rt_probe_backend(MotorProbeReport* report);
void motor_rt_init_backend();
bool motor_rt_do_comm_backend();
bool motor_rt_off_backend();
bool motor_rt_calibrate_backend();
void motor_rt_sleep_backend();

extern const MotorBackendDefinition MOTOR_BACKEND_RT;
