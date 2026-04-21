#pragma once

#include "motor_hal.h"

void motor_jmc_init_backend();
bool motor_jmc_do_comm_backend();
bool motor_jmc_off_backend();
bool motor_jmc_calibrate_backend();
void motor_jmc_sleep_backend();
void motor_jmc_reset_backend();
