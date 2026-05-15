#pragma once

#include "motor_backend.h"

struct MotorProbeReport;

bool motor_krs_130st_m07725_probe_backend(MotorProbeReport* report);
bool motor_krs_xb130aea20025_shl_probe_backend(MotorProbeReport* report);
void motor_krs_130st_m07725_init_backend();
bool motor_krs_130st_m07725_do_comm_backend();
bool motor_krs_130st_m07725_off_backend();
bool motor_krs_130st_m07725_calibrate_backend();
void motor_krs_130st_m07725_sleep_backend();
void motor_krs_130st_m07725_reset_backend();

extern const MotorBackendDefinition MOTOR_BACKEND_KRS_130ST_M07725;
extern const MotorBackendDefinition MOTOR_BACKEND_KRS_XB130AEA20025_SHL;
