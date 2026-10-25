#pragma once
#include "comm.h"
bool motor_probe_set_profile(CommProfile profile);
bool motor_probe_read_register(uint16_t reg, uint16_t count, uint8_t retries,
                               uint16_t* values, uint16_t maxValues);
