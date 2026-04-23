#pragma once

#include "comm.h"

bool motor_probe_set_profile(CommProfile profile);
bool motor_probe_read_register(uint16_t regAddr,
                               uint16_t regCount,
                               uint8_t retries,
                               uint16_t* values = nullptr,
                               uint16_t maxValues = 0);
bool motor_probe_write_register(uint16_t regAddr, uint16_t value);

JmcCommRate motor_probe_jmc_rate_from_baud(uint32_t baud);
uint32_t motor_probe_baud_from_jmc_rate(JmcCommRate rate);
uint8_t motor_probe_build_jmc_order(JmcCommRate preferredRate, JmcCommRate* rates, uint8_t maxCount);
