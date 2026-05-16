#include "SEW/motorcontrol_sew_movilink.h"

#include <limits.h>

#include "SEW/movilink.h"
#include "comm.h"
#include "display.h"
#include "error_history.h"
#include "main.h"
#include "motor_hal.h"
#include "motor_probe.h"
#include "pdata.h"

namespace {
constexpr uint8_t SEW_PROBE_RETRIES = 2;
constexpr uint16_t SEW_CONTROL_RAPID_STOP = 0x0000;
constexpr uint16_t SEW_CONTROL_ENABLE = 0x0006;
constexpr uint16_t SEW_CONTROL_FAULT_RESET = 0x0040;
constexpr uint16_t SEW_PD_RESPONSE_TIMEOUT_MS = 70;
constexpr unsigned long SEW_RPM_LOG_ZERO_DELAY_MS = 2000;

const uint8_t SEW_ADDRESS_CANDIDATES[] = {0, 1, 2};

uint8_t sewAddress = SEW_MOVILINK_DEFAULT_ADDRESS;
uint8_t awaitingAddress = SEW_MOVILINK_DEFAULT_ADDRESS;
uint8_t candidateAddressIndex = 0;
bool awaitingResponse = false;
unsigned long lastCommSend = 0;
unsigned long lastCommReceived = 0;
unsigned long awaitingResponseSince = 0;
uint16_t lastStatusWord = 0;
uint8_t pendingResetFrames = 0;
unsigned long sewRpmSessionStartedAt = 0;
bool sewRpmLoggedSample = false;
bool sewRpmLoggedZero = false;

bool sewRpmLogStateActive() {
    switch(state) {
        case GRINDING:
        case PURGING:
        case GRINDING_GBW:
        case CALIBRATING:
            return true;
        default:
            return false;
    }
}

uint32_t packMotorRpmLogValue(uint8_t subtype, uint8_t source, int16_t value) {
    return (static_cast<uint32_t>(subtype) << 24) |
           (static_cast<uint32_t>(source) << 16) |
           static_cast<uint16_t>(value);
}

void resetSewRpmLogSession() {
    sewRpmSessionStartedAt = 0;
    sewRpmLoggedSample = false;
    sewRpmLoggedZero = false;
}

int16_t clampToInt16(long value) {
    if(value > INT16_MAX) return INT16_MAX;
    if(value < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(value);
}

int16_t encodeSewRpm(int16_t rpm) {
    return clampToInt16(static_cast<long>(rpm) * 5L);
}

int16_t decodeSewRpm(uint16_t raw) {
    return static_cast<int16_t>(raw) / 5;
}

uint16_t sewCurrentLimitWord() {
    const int16_t configured = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, 1);
    return configured > 0 ? static_cast<uint16_t>(configured) : 0;
}

uint16_t sewControlWordForSetpoint(int16_t effectiveSetRpm) {
    if(pendingResetFrames > 0) {
        return SEW_CONTROL_FAULT_RESET;
    }

    return effectiveSetRpm == 0 ? SEW_CONTROL_RAPID_STOP : SEW_CONTROL_ENABLE;
}

uint8_t nextDisconnectedAddress() {
    return SEW_ADDRESS_CANDIDATES[candidateAddressIndex];
}

void advanceDisconnectedAddress() {
    candidateAddressIndex++;
    if(candidateAddressIndex >= sizeof(SEW_ADDRESS_CANDIDATES)) {
        candidateAddressIndex = 0;
    }
}

void markConnected(uint8_t address) {
    sewAddress = address;
    lastCommReceived = millis();
    lastReceived = lastCommReceived;
    commStatus = COMM_CONNECTED;
    commCounter = 0;
    if(currentStatus == MOTOR_NOT_CONNECTED) {
        currentStatus = MOTOR_NOT_READY;
    }
}

void updateCurrentStatusFromSew(uint16_t statusWord) {
    if((statusWord & (1U << 5)) != 0) {
        currentStatus = MOTOR_FAULT;
        error = 3;
        errorCode = statusWord >> 8;
        return;
    }

    if((statusWord & (1U << 0)) != 0) {
        currentStatus = MOTOR_ENABLED;
    } else if((statusWord & (1U << 1)) != 0) {
        currentStatus = MOTOR_READY;
    } else if(commStatus == COMM_CONNECTED) {
        currentStatus = MOTOR_NOT_READY;
    } else {
        currentStatus = MOTOR_NOT_CONNECTED;
    }
}

void noteSewRpmRead(int16_t rpm) {
    if(!sewRpmLogStateActive() || motor_setRPM == 0) {
        return;
    }

    if(sewRpmSessionStartedAt == 0) {
        sewRpmSessionStartedAt = millis();
    }

    if(!sewRpmLoggedSample && rpm != 0) {
        error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                          ERROR_HISTORY_LEVEL_INFO,
                          ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                          packMotorRpmLogValue(ERROR_HISTORY_MOTOR_RPM_SAMPLE,
                                               ERROR_HISTORY_MOTOR_RPM_SRC_SEW_PI2,
                                               rpm),
                          state);
        sewRpmLoggedSample = true;
    }
}

void checkSewRpmZeroLog() {
    if(!sewRpmLogStateActive() || motor_setRPM == 0) {
        resetSewRpmLogSession();
        return;
    }

    if(sewRpmSessionStartedAt == 0) {
        sewRpmSessionStartedAt = millis();
    }

    if(!sewRpmLoggedSample && !sewRpmLoggedZero &&
       millis() - sewRpmSessionStartedAt >= SEW_RPM_LOG_ZERO_DELAY_MS &&
       motor_currentRPM == 0) {
        error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                          ERROR_HISTORY_LEVEL_WARN,
                          ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                          packMotorRpmLogValue(ERROR_HISTORY_MOTOR_RPM_ZERO,
                                               ERROR_HISTORY_MOTOR_RPM_SRC_SEW_PI2,
                                               motor_get_effective_set_rpm()),
                          state);
        sewRpmLoggedZero = true;
    }
}

bool handleSewResponse() {
    if(!awaitingResponse) {
        return false;
    }

    SewMovilinkPd3Response response = {};
    if(sew_movilink_read_pd3_response(awaitingAddress, &response)) {
        awaitingResponse = false;
        if(commCounter > 0) {
            commCounter--;
        }

        if(error == 2 || error == 1) {
            error = 0;
            disp_updateRequired = true;
        }

        markConnected(response.address);
        lastStatusWord = response.word1;
        motor_currentRPM = decodeSewRpm(response.word2);
        motor_currentTorque = abs(static_cast<int16_t>(response.word3));
        updateCurrentStatusFromSew(lastStatusWord);
        noteSewRpmRead(motor_currentRPM);
        newData = true;
        return true;
    }

    if(millis() - awaitingResponseSince >= SEW_PD_RESPONSE_TIMEOUT_MS) {
        awaitingResponse = false;
        if(commStatus == COMM_DISCONNECTED) {
            advanceDisconnectedAddress();
        }
    }

    return false;
}

bool sendSewCommand(uint8_t address) {
    const int16_t effectiveSetRpm = motor_get_effective_set_rpm();
    const uint16_t controlWord = sewControlWordForSetpoint(effectiveSetRpm);
    const uint16_t speedWord = static_cast<uint16_t>(encodeSewRpm(effectiveSetRpm));

    if(!sew_movilink_send_pd3(address, controlWord, speedWord, sewCurrentLimitWord())) {
        return false;
    }

    if(pendingResetFrames > 0) {
        pendingResetFrames--;
    }

    awaitingAddress = address;
    awaitingResponse = true;
    awaitingResponseSince = millis();
    lastCommSend = awaitingResponseSince;
    return true;
}

bool probeSewAddress(uint8_t address) {
    for(uint8_t attempt = 0; attempt < SEW_PROBE_RETRIES; ++attempt) {
        SewMovilinkPd3Response response = {};
        if(sew_movilink_exchange_pd3(address,
                                     SEW_CONTROL_RAPID_STOP,
                                     0,
                                     0,
                                     &response,
                                     SEW_PD_RESPONSE_TIMEOUT_MS)) {
            sewAddress = response.address;
            return true;
        }
        commCounter = 0;
        delay(40);
    }
    return false;
}
}

void motor_sew_movilink_init_backend() {
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    sewAddress = SEW_MOVILINK_DEFAULT_ADDRESS;
    awaitingAddress = sewAddress;
    candidateAddressIndex = 0;
    awaitingResponse = false;
    lastCommSend = 0;
    lastCommReceived = 0;
    awaitingResponseSince = 0;
    lastStatusWord = 0;
    pendingResetFrames = 0;
    resetSewRpmLogSession();
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
}

bool motor_sew_movilink_probe_backend(MotorProbeReport* report) {
    if(report == nullptr) {
        return false;
    }

    report->sew_movilink_3pd = false;
    report->sew_movilink_address = SEW_MOVILINK_DEFAULT_ADDRESS;
    motor_probe_set_profile(COMM_PROFILE_SEW_MOVILINK);

    for(uint8_t address : SEW_ADDRESS_CANDIDATES) {
        if(probeSewAddress(address)) {
            report->sew_movilink_3pd = true;
            report->sew_movilink_address = sewAddress;
            return true;
        }
    }

    return false;
}

bool motor_sew_movilink_do_comm_backend() {
    if(handleSewResponse()) {
        return true;
    }

    if(lastCommReceived > 0 && lastCommReceived + DISCONNECTED_AFTER < millis()) {
        #ifndef DEBUG
            error = 2;
        #endif
        commStatus = COMM_DISCONNECTED;
        currentStatus = MOTOR_NOT_CONNECTED;
        disp_updateRequired = true;
        awaitingResponse = false;
    }

    if(lastCommReceived == 0 && commStatus == COMM_DISCONNECTED && millis() > DISCONNECTED_AFTER) {
        #ifndef DEBUG
            error = 1;
        #endif
        currentStatus = MOTOR_NOT_CONNECTED;
        disp_updateRequired = true;
    }

    if(commStatus != COMM_DISCONNECTED && commCounter > 25) {
        error = 4;
        motor_setRPM = 0;
        state = IDLE;
        disp_updateRequired = true;
    }

    const int16_t torquePercent = motor_get_torque_percent();
    if(motor_setRPM != 0 && torquePercent > 90 && abs(motor_currentRPM) < rpm_scalar) {
        motor_setRPM = 0;
        state = IDLE;
        error = 6;
    }

    checkSewRpmZeroLog();

    const uint16_t sendDelay = commStatus == COMM_DISCONNECTED
                                   ? (controller_is_menu_state(state)
                                          ? comm_get_menu_delay_ms()
                                          : comm_get_send_delay_ms())
                                   : comm_get_active_interval_ms();
    if(lastCommSend + sendDelay > millis()) {
        return false;
    }

    newData = false;
    const uint8_t address = commStatus == COMM_CONNECTED ? sewAddress : nextDisconnectedAddress();
    sendSewCommand(address);
    return false;
}

bool motor_sew_movilink_off_backend() {
    motor_setRPM = 0;
    uint8_t score = 0;
    for(uint8_t i = 0; i < 3; ++i) {
        SewMovilinkPd3Response response = {};
        if(sew_movilink_exchange_pd3(sewAddress,
                                     SEW_CONTROL_RAPID_STOP,
                                     0,
                                     sewCurrentLimitWord(),
                                     &response,
                                     SEW_PD_RESPONSE_TIMEOUT_MS)) {
            markConnected(response.address);
            score++;
            break;
        }
        delay(80);
    }
    currentStatus = commStatus == COMM_CONNECTED ? MOTOR_READY : MOTOR_NOT_CONNECTED;
    return score > 0;
}

void motor_sew_movilink_sleep_backend() {
    motor_sew_movilink_off_backend();
}

void motor_sew_movilink_reset_backend() {
    pendingResetFrames = 2;
    SewMovilinkPd3Response response = {};
    if(sew_movilink_exchange_pd3(sewAddress,
                                 SEW_CONTROL_FAULT_RESET,
                                 0,
                                 sewCurrentLimitWord(),
                                 &response,
                                 SEW_PD_RESPONSE_TIMEOUT_MS)) {
        markConnected(response.address);
    }
}

bool motor_sew_movilink_calibrate_backend() {
    static unsigned long lastCall;
    static unsigned long checkTime;
    static uint32_t sum = 0;
    static uint16_t sumCount = 0;

    if(lastCall + 3000 < millis()) {
        currentCal = 0;
        sum = 0;
        sumCount = 0;
        checkTime = millis();
    }

    lastCall = millis();

    if(currentCal == 0 && Menu1[SETMIN].value == 0) {
        calibrateArray[0] = 0;
        currentCal = 1;
        checkTime = millis();
    }

    if(commStatus != COMM_CONNECTED) {
        error = 101;
        state = MENU1;
        return false;
    }

    if(checkTime + 250 < millis() && newData == true) {
        sum = sum + motor_currentTorque;
        sumCount++;
    }

    if(currentCal < (SIZEOFCALIBRATEARRAY / sizeof(uint16_t)) &&
       checkTime + CAL_TIME < millis() &&
       sumCount != 0) {
        calibrateArray[currentCal] = uint16_t(sum / sumCount);
        currentCal++;
        motor_setRPM = (currentCal * rpm_scalar + absolute_min_rpm) > absolute_max_rpm
                           ? absolute_max_rpm
                           : (currentCal * rpm_scalar + absolute_min_rpm);
        checkTime = millis();
        sum = 0;
        sumCount = 0;
        disp_updateRequired = true;
    }

    if(checkTime + CAL_TIME < millis() && currentCal >= (SIZEOFCALIBRATEARRAY / sizeof(uint16_t))) {
        if(sumCount != 0) {
            calibrateArray[currentCal] = uint16_t(sum / sumCount);
        }
        motor_setRPM = 0;
        pdata_write(1);
        state = IDLE;
        enterMenu = false;
        menu1Selected = EXITMENU;
        return true;
    }

    return false;
}

const MotorBackendDefinition MOTOR_BACKEND_SEW_MOVILINK = {
    "sew-movilink",
    "SEW MOVILINK backend",
    "SEW MOVITRAC/MOVIDRIVE RS-485 backend using MOVILINK 3 process-data telegrams.",
    "Probe 9600 8E1 MOVILINK 3PD with zero command words at addresses 0, 1, and 2.",
    "Requires PO1=control word 1, PO2=speed setpoint, PO3=current limit, and PO data enabled on the drive.",
    "Expects PI1=status word 1, PI2=actual speed, and PI3=actual current; speed uses 0.2 rpm/count and current uses 0.1% In/count.",
    {
        motor_sew_movilink_probe_backend,
        motor_sew_movilink_init_backend,
        motor_sew_movilink_do_comm_backend,
        motor_sew_movilink_off_backend,
        motor_sew_movilink_calibrate_backend,
        motor_sew_movilink_sleep_backend,
        motor_sew_movilink_reset_backend,
    },
};
