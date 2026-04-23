#pragma once

#include "motor_backend.h"

struct MotorProbeReport;

bool motor_jmc_probe_backend(MotorProbeReport* report);
void motor_jmc_init_backend();
bool motor_jmc_do_comm_backend();
bool motor_jmc_off_backend();
bool motor_jmc_calibrate_backend();
void motor_jmc_sleep_backend();
void motor_jmc_reset_backend();

extern const MotorBackendDefinition MOTOR_BACKEND_JMC;
