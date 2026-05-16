#include "SEW/movilink.h"

#include <HardwareSerial.h>

#include "comm.h"
#include "motor_hal.h"

namespace {
constexpr uint8_t SEW_MOVILINK_PD3_FRAME_LEN = 10;
constexpr uint8_t SEW_MOVILINK_PARAM_FRAME_LEN = 12;
unsigned long lastMovilinkSendAt = 0;

bool sendFrame(const uint8_t* frame, uint8_t length) {
    if(frame == nullptr || length == 0) {
        return false;
    }

    if(lastMovilinkSendAt + comm_get_send_delay_ms() > millis()) {
        return false;
    }

    while(Serial2.available()) {
        Serial2.read();
    }

    setTXmode();
    delayMicroseconds(50);
    Serial2.write(frame, length);
    Serial2.flush();
    delayMicroseconds(50);
    setRXmode();

    lastMovilinkSendAt = millis();
    commCounter++;
    return true;
}

bool readFrame(uint8_t expectedAddress,
               uint8_t expectedType,
               uint8_t expectedLength,
               uint8_t* frame,
               uint8_t frameLen) {
    if(frame == nullptr || frameLen < expectedLength) {
        return false;
    }

    while(Serial2.available() > 0 && Serial2.peek() != SEW_MOVILINK_RESPONSE_START) {
        Serial2.read();
    }

    if(Serial2.available() < expectedLength) {
        return false;
    }

    Serial2.readBytes(frame, expectedLength);
    if(frame[0] != SEW_MOVILINK_RESPONSE_START ||
       frame[1] != expectedAddress ||
       frame[2] != expectedType) {
        return false;
    }

    return sew_movilink_bcc(frame, expectedLength - 1) == frame[expectedLength - 1];
}

bool sendParameterRead(uint8_t address, uint16_t index, uint8_t subindex) {
    uint8_t frame[SEW_MOVILINK_PARAM_FRAME_LEN] = {
        SEW_MOVILINK_REQUEST_START,
        address,
        SEW_MOVILINK_TYPE_PARAM_0PD_ACYCLIC,
        0x31,
        subindex,
        static_cast<uint8_t>(index >> 8),
        static_cast<uint8_t>(index & 0xFF),
        0,
        0,
        0,
        0,
        0,
    };
    frame[SEW_MOVILINK_PARAM_FRAME_LEN - 1] =
        sew_movilink_bcc(frame, SEW_MOVILINK_PARAM_FRAME_LEN - 1);
    return sendFrame(frame, SEW_MOVILINK_PARAM_FRAME_LEN);
}
}

uint8_t sew_movilink_bcc(const uint8_t* data, uint8_t length) {
    uint8_t bcc = 0;
    if(data == nullptr) {
        return bcc;
    }

    for(uint8_t i = 0; i < length; ++i) {
        bcc ^= data[i];
    }
    return bcc;
}

bool sew_movilink_send_pd3(uint8_t address, uint16_t po1, uint16_t po2, uint16_t po3) {
    uint8_t frame[SEW_MOVILINK_PD3_FRAME_LEN] = {
        SEW_MOVILINK_REQUEST_START,
        address,
        SEW_MOVILINK_TYPE_3PD,
        static_cast<uint8_t>(po1 >> 8),
        static_cast<uint8_t>(po1 & 0xFF),
        static_cast<uint8_t>(po2 >> 8),
        static_cast<uint8_t>(po2 & 0xFF),
        static_cast<uint8_t>(po3 >> 8),
        static_cast<uint8_t>(po3 & 0xFF),
        0,
    };
    frame[SEW_MOVILINK_PD3_FRAME_LEN - 1] =
        sew_movilink_bcc(frame, SEW_MOVILINK_PD3_FRAME_LEN - 1);
    return sendFrame(frame, SEW_MOVILINK_PD3_FRAME_LEN);
}

bool sew_movilink_read_pd3_response(uint8_t address, SewMovilinkPd3Response* out) {
    uint8_t frame[SEW_MOVILINK_PD3_FRAME_LEN] = {};
    if(!readFrame(address,
                  SEW_MOVILINK_TYPE_3PD,
                  SEW_MOVILINK_PD3_FRAME_LEN,
                  frame,
                  sizeof(frame))) {
        return false;
    }

    if(out != nullptr) {
        out->address = frame[1];
        out->word1 = static_cast<uint16_t>((frame[3] << 8) | frame[4]);
        out->word2 = static_cast<uint16_t>((frame[5] << 8) | frame[6]);
        out->word3 = static_cast<uint16_t>((frame[7] << 8) | frame[8]);
    }
    return true;
}

bool sew_movilink_exchange_pd3(uint8_t address,
                               uint16_t po1,
                               uint16_t po2,
                               uint16_t po3,
                               SewMovilinkPd3Response* out,
                               uint16_t timeoutMs) {
    const unsigned long sendDeadline = millis() + comm_get_send_delay_ms() + 20;
    while(!sew_movilink_send_pd3(address, po1, po2, po3)) {
        if(millis() >= sendDeadline) {
            return false;
        }
        delay(1);
    }

    const unsigned long receiveDeadline = millis() + timeoutMs;
    while(millis() < receiveDeadline) {
        if(sew_movilink_read_pd3_response(address, out)) {
            if(commCounter > 0) {
                commCounter--;
            }
            return true;
        }
        delay(1);
    }
    return false;
}

bool sew_movilink_read_parameter(uint8_t address,
                                 uint16_t index,
                                 uint8_t subindex,
                                 uint32_t* value,
                                 SewMovilinkParameterResult* result,
                                 uint16_t timeoutMs) {
    const unsigned long sendDeadline = millis() + comm_get_send_delay_ms() + 20;
    while(!sendParameterRead(address, index, subindex)) {
        if(millis() >= sendDeadline) {
            return false;
        }
        delay(1);
    }

    uint8_t frame[SEW_MOVILINK_PARAM_FRAME_LEN] = {};
    const unsigned long receiveDeadline = millis() + timeoutMs;
    while(millis() < receiveDeadline) {
        if(readFrame(address,
                     SEW_MOVILINK_TYPE_PARAM_0PD_ACYCLIC,
                     SEW_MOVILINK_PARAM_FRAME_LEN,
                     frame,
                     sizeof(frame))) {
            SewMovilinkParameterResult parsed = {};
            parsed.address = frame[1];
            parsed.management = frame[3];
            parsed.subindex = frame[4];
            parsed.index = static_cast<uint16_t>((frame[5] << 8) | frame[6]);
            parsed.data = (static_cast<uint32_t>(frame[7]) << 24) |
                          (static_cast<uint32_t>(frame[8]) << 16) |
                          (static_cast<uint32_t>(frame[9]) << 8) |
                          static_cast<uint32_t>(frame[10]);
            parsed.statusError = (parsed.management & 0x80U) != 0;
            if(parsed.statusError) {
                parsed.errorClass = frame[7];
                parsed.errorCode = frame[8];
                parsed.additionalCode =
                    static_cast<uint16_t>((frame[9] << 8) | frame[10]);
            } else if(value != nullptr) {
                *value = parsed.data;
            }

            if(result != nullptr) {
                *result = parsed;
            }
            if(commCounter > 0) {
                commCounter--;
            }
            return !parsed.statusError && parsed.index == index && parsed.subindex == subindex;
        }
        delay(1);
    }

    return false;
}
