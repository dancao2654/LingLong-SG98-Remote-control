#include "motor_probe.h"

#include <HardwareSerial.h>

#include "espnow_control.h"
#include "motor_hal.h"

namespace {
constexpr unsigned long PROFILE_SETTLE_MS = 20;
constexpr unsigned long BETWEEN_PROBE_ATTEMPTS_MS = 40;

void serviceProbeLink() {
    static unsigned long lastServiceAt = 0;
    const unsigned long now = millis();
    if(now - lastServiceAt >= 100) {
        espnow_control_poll_probe_status();
        lastServiceAt = now;
    }
}
}

bool motor_probe_set_profile(CommProfile profile) {
    const bool changed = comm_set_profile(profile);
    if(changed) {
        delay(PROFILE_SETTLE_MS);
    }
    return changed;
}

bool motor_probe_read_register(uint16_t regAddr,
                               uint16_t regCount,
                               uint8_t retries,
                               uint16_t* values,
                               uint16_t maxValues) {
    for(uint8_t attempt = 0; attempt < retries; ++attempt) {
        while(Serial2.available()) Serial2.read();

        const unsigned long sendDeadline = millis() + comm_get_send_delay_ms() + 20;
        while(!readRegister(regAddr, regCount)) {
            if(millis() >= sendDeadline) break;
            serviceProbeLink();
            delay(1);
        }

        const unsigned long receiveDeadline = millis() + comm_get_receive_delay_ms() + 250;
        while(millis() < receiveDeadline) {
            const int16_t received = receive();
            if(received == regAddr) {
                if(values != nullptr && maxValues > 0) {
                    const uint16_t copyCount = (regCount < maxValues) ? regCount : maxValues;
                    for(uint16_t i = 0; i < copyCount; ++i) {
                        values[i] = lastRead[i];
                    }
                }
                commCounter = 0;
                return true;
            }
            serviceProbeLink();
            delay(1);
        }

        while(Serial2.available()) Serial2.read();
        commCounter = 0;
        serviceProbeLink();
        delay(BETWEEN_PROBE_ATTEMPTS_MS);
    }

    return false;
}

bool motor_probe_write_register(uint16_t regAddr, uint16_t value) {
    const bool success = writeConfirm(regAddr, value);
    commCounter = 0;
    return success;
}

JmcCommRate motor_probe_jmc_rate_from_baud(uint32_t baud) {
    switch(baud) {
        case 57600:
            return JMC_COMM_RATE_57600;
        case 38400:
            return JMC_COMM_RATE_38400;
        case 19200:
            return JMC_COMM_RATE_19200;
        case 115200:
            return JMC_COMM_RATE_115200;
        case 9600:
        default:
            return JMC_COMM_RATE_9600;
    }
}

uint32_t motor_probe_baud_from_jmc_rate(JmcCommRate rate) {
    switch(rate) {
        case JMC_COMM_RATE_57600:
            return 57600;
        case JMC_COMM_RATE_38400:
            return 38400;
        case JMC_COMM_RATE_19200:
            return 19200;
        case JMC_COMM_RATE_115200:
            return 115200;
        case JMC_COMM_RATE_9600:
        default:
            return 9600;
    }
}

uint8_t motor_probe_build_jmc_order(JmcCommRate preferredRate, JmcCommRate* rates, uint8_t maxCount) {
    if(rates == nullptr || maxCount == 0) {
        return 0;
    }

    const JmcCommRate candidates[] = {
        preferredRate,
        JMC_COMM_RATE_9600,
        JMC_COMM_RATE_19200,
        JMC_COMM_RATE_38400,
        JMC_COMM_RATE_57600,
        JMC_COMM_RATE_115200,
    };

    uint8_t count = 0;
    for(JmcCommRate candidate : candidates) {
        bool alreadyAdded = false;
        for(uint8_t i = 0; i < count; ++i) {
            if(rates[i] == candidate) {
                alreadyAdded = true;
                break;
            }
        }
        if(alreadyAdded) {
            continue;
        }
        rates[count++] = candidate;
        if(count >= maxCount) {
            break;
        }
    }

    return count;
}
