#pragma once
#include <cstdint>

enum CommProfile { COMM_PROFILE_TSE };
constexpr uint8_t SLAVE_ID = 1;
extern uint16_t lastRead[16];
extern uint8_t lastRequestType;
uint32_t millis();
bool readRegister(uint16_t reg, uint16_t count);
bool writeSingleRegister(uint16_t reg, uint16_t value);
bool writeConfirm(uint16_t reg, uint16_t value);
int16_t receive();
uint16_t comm_get_active_interval_ms();
