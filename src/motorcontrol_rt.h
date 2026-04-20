#pragma once

#include "motor_hal.h"

void motor_rt_init_backend();
bool motor_rt_do_comm_backend();
bool motor_rt_off_backend();
bool motor_rt_calibrate_backend();
void motor_rt_sleep_backend();
