#pragma once

#include <Arduino.h>

constexpr uint8_t SEW_MOVILINK_DEFAULT_ADDRESS = 0;
constexpr uint8_t SEW_MOVILINK_REQUEST_START = 0x02;
constexpr uint8_t SEW_MOVILINK_RESPONSE_START = 0x1D;
constexpr uint8_t SEW_MOVILINK_TYPE_3PD = 0x05;
constexpr uint8_t SEW_MOVILINK_TYPE_PARAM_0PD_ACYCLIC = 0x86;

struct SewMovilinkPd3Response {
    uint8_t address = 0;
    uint16_t word1 = 0;
    uint16_t word2 = 0;
    uint16_t word3 = 0;
};

struct SewMovilinkParameterResult {
    uint8_t address = 0;
    uint8_t management = 0;
    uint8_t subindex = 0;
    uint16_t index = 0;
    uint32_t data = 0;
    uint8_t errorClass = 0;
    uint8_t errorCode = 0;
    uint16_t additionalCode = 0;
    bool statusError = false;
};

uint8_t sew_movilink_bcc(const uint8_t* data, uint8_t length);
bool sew_movilink_send_pd3(uint8_t address, uint16_t po1, uint16_t po2, uint16_t po3);
bool sew_movilink_read_pd3_response(uint8_t address, SewMovilinkPd3Response* out);
bool sew_movilink_exchange_pd3(uint8_t address,
                               uint16_t po1,
                               uint16_t po2,
                               uint16_t po3,
                               SewMovilinkPd3Response* out,
                               uint16_t timeoutMs = 70);
bool sew_movilink_read_parameter(uint8_t address,
                                 uint16_t index,
                                 uint8_t subindex,
                                 uint32_t* value,
                                 SewMovilinkParameterResult* result = nullptr,
                                 uint16_t timeoutMs = 90);
