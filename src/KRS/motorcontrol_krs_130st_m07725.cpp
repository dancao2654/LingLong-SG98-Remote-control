#include "motorcontrol_krs_130st_m07725.h"

#include "comm.h"
#include "display.h"
#include "main.h"
#include "motor_hal.h"
#include "motor_probe.h"
#include "pdata.h"

namespace {
constexpr uint16_t KRS_REG_MOTOR_CODE = 0x0001;          // Pn001
constexpr uint16_t KRS_REG_CONTROL_MODE = 0x0002;        // Pn002, 1 = speed mode
constexpr uint16_t KRS_REG_TORQUE_LIMIT_CCW = 0x0008;    // Pn008
constexpr uint16_t KRS_REG_TORQUE_LIMIT_CW = 0x0009;     // Pn009
constexpr uint16_t KRS_REG_INPUT_CONTROL_1 = 0x0044;     // Pn068
constexpr uint16_t KRS_REG_INPUT_LOGIC_1 = 0x0046;       // Pn070
constexpr uint16_t KRS_REG_SPEED_SOURCE = 0x00A8;        // Pn168
constexpr uint16_t KRS_REG_SPEED_REFERENCE_1 = 0x00A9;   // Pn169
constexpr uint16_t KRS_REG_MONITOR_DN01 = 0x0171;        // Dn001..Dn008

constexpr uint16_t KRS_MOTOR_CODE_130ST_M07725 = 20;
constexpr uint16_t KRS_SPEED_MODE = 1;
constexpr uint16_t KRS_SPEED_SOURCE_INTERNAL_1_TO_8 = 1;
constexpr uint16_t KRS_COMM_CONTROL_SON_AND_SP = 0x0701;
constexpr uint16_t KRS_INPUT_LOGIC_DISABLED = 0x78B2;
constexpr uint16_t KRS_INPUT_LOGIC_ENABLED_INTERNAL_SPEED_1 = 0x78B3;
constexpr uint8_t KRS_PROBE_RETRIES = 2;
constexpr uint8_t KRS_MONITOR_COUNT = 8;

uint16_t motor_currentStatus = 0;
int16_t reg_control_mode = -1;
int16_t reg_torque_limit_ccw = -1;
int16_t reg_torque_limit_cw = -1;
int16_t reg_input_control = -1;
int16_t reg_input_logic = -1;
int16_t reg_speed_source = -1;
int16_t reg_speed_reference = -1;
unsigned long lastCommSend = 0;
unsigned long lastCommReceived = 0;
uint8_t nextCommType = 0;
int16_t motor_lastSetRPM = 0;

void resetKrsRegisters() {
    reg_control_mode = -1;
    reg_torque_limit_ccw = -1;
    reg_torque_limit_cw = -1;
    reg_input_control = -1;
    reg_input_logic = -1;
    reg_speed_source = -1;
    reg_speed_reference = -1;
}

bool probeKrsMotorCode(MotorProbeReport* report) {
    if(report == nullptr) {
        return false;
    }

    uint16_t values[1] = {0};
    motor_probe_set_profile(COMM_PROFILE_KRS);
    report->krs_130st_m07725_motor_code =
        motor_probe_read_register(KRS_REG_MOTOR_CODE, 1, KRS_PROBE_RETRIES, values, 1) &&
        values[0] == KRS_MOTOR_CODE_130ST_M07725;
    return report->krs_130st_m07725_motor_code;
}

void markConnected() {
    lastCommReceived = millis();
    commStatus = COMM_CONNECTED;
    if(currentStatus == MOTOR_NOT_CONNECTED) {
        currentStatus = MOTOR_NOT_READY;
    }
    commCounter = 0;
}

bool configureKrsDrive() {
    uint8_t score = 0;

    if(writeConfirm(KRS_REG_CONTROL_MODE, KRS_SPEED_MODE)) {
        reg_control_mode = KRS_SPEED_MODE;
        score++;
    }

    delay(50);
    if(writeConfirm(KRS_REG_SPEED_SOURCE, KRS_SPEED_SOURCE_INTERNAL_1_TO_8)) {
        reg_speed_source = KRS_SPEED_SOURCE_INTERNAL_1_TO_8;
        score++;
    }

    delay(50);
    if(writeConfirm(KRS_REG_INPUT_CONTROL_1, KRS_COMM_CONTROL_SON_AND_SP)) {
        reg_input_control = KRS_COMM_CONTROL_SON_AND_SP;
        score++;
    }

    delay(50);
    if(writeConfirm(KRS_REG_SPEED_REFERENCE_1, 0)) {
        reg_speed_reference = 0;
        score++;
    }

    delay(50);
    if(writeConfirm(KRS_REG_INPUT_LOGIC_1, KRS_INPUT_LOGIC_DISABLED)) {
        reg_input_logic = KRS_INPUT_LOGIC_DISABLED;
        score++;
    }

    return score == 5;
}

void updateCurrentStatusFromKrs() {
    if(reg_input_logic == KRS_INPUT_LOGIC_ENABLED_INTERNAL_SPEED_1) {
        currentStatus = MOTOR_ENABLED;
    } else if(commStatus == COMM_CONNECTED) {
        currentStatus = MOTOR_READY;
    } else {
        currentStatus = MOTOR_NOT_CONNECTED;
    }
}
}

void motor_krs_130st_m07725_init_backend() {
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    motor_currentStatus = 0;
    resetKrsRegisters();
    lastCommSend = 0;
    lastCommReceived = 0;
    nextCommType = 0;
    motor_lastSetRPM = 0;
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
}

bool motor_krs_130st_m07725_probe_backend(MotorProbeReport* report) {
    if(report == nullptr) {
        return false;
    }

    report->krs_130st_m07725_motor_code = false;
    return probeKrsMotorCode(report);
}

bool motor_krs_130st_m07725_do_comm_backend() {
    const int16_t configuredTorque = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, 1);
    const int16_t configuredReverseTorque = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, -1);

    if(lastCommReceived > 0 && lastCommReceived + DISCONNECTED_AFTER < millis()) {
        #ifndef DEBUG
            error = 2;
        #endif
        commStatus = COMM_DISCONNECTED;
        currentStatus = MOTOR_NOT_CONNECTED;
        disp_updateRequired = true;
        resetKrsRegisters();
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

    if(commStatus == COMM_DISCONNECTED) {
        const uint16_t reconnectDelay = controller_is_menu_state(state) ? comm_get_menu_delay_ms() : comm_get_send_delay_ms();
        if(lastCommSend + reconnectDelay > millis()) {
            return false;
        }

        if(configureKrsDrive()) {
            markConnected();
            nextCommType = 0;
            currentStatus = MOTOR_NOT_READY;
        }
        lastCommSend = millis();
        return false;
    }

    if(motor_setRPM == 0 && motor_lastSetRPM != 0) {
        if(reg_speed_reference != 0) {
            if(writeConfirm(KRS_REG_SPEED_REFERENCE_1, 0)) {
                reg_speed_reference = 0;
            }
        }
        if(reg_input_logic != KRS_INPUT_LOGIC_DISABLED) {
            if(writeConfirm(KRS_REG_INPUT_LOGIC_1, KRS_INPUT_LOGIC_DISABLED)) {
                reg_input_logic = KRS_INPUT_LOGIC_DISABLED;
            }
        }
        motor_lastSetRPM = motor_setRPM;
        nextCommType = 0;
        return false;
    }

    int16_t received = receive();
    if(received > 0) {
        if(error == 2 || error == 1) {
            error = 0;
            disp_updateRequired = true;
        }

        markConnected();

        if(lastRequestType == 0) {
            switch(received) {
                case KRS_REG_CONTROL_MODE: reg_control_mode = static_cast<int16_t>(lastRead[0]); break;
                case KRS_REG_TORQUE_LIMIT_CCW: reg_torque_limit_ccw = static_cast<int16_t>(lastRead[0]); break;
                case KRS_REG_TORQUE_LIMIT_CW: reg_torque_limit_cw = static_cast<int16_t>(lastRead[0]); break;
                case KRS_REG_INPUT_CONTROL_1: reg_input_control = static_cast<int16_t>(lastRead[0]); break;
                case KRS_REG_INPUT_LOGIC_1: reg_input_logic = static_cast<int16_t>(lastRead[0]); break;
                case KRS_REG_SPEED_SOURCE: reg_speed_source = static_cast<int16_t>(lastRead[0]); break;
                case KRS_REG_SPEED_REFERENCE_1: reg_speed_reference = static_cast<int16_t>(lastRead[0]); break;
            }
        }

        if(lastRequestType > 100 && lastRequestType < 200 && received == KRS_REG_MONITOR_DN01) {
            motor_currentTorque = static_cast<int16_t>(lastRead[1]);
            motor_currentRPM = static_cast<int16_t>(lastRead[7]);
            motor_currentStatus = static_cast<uint16_t>(reg_input_logic);
            updateCurrentStatusFromKrs();
            newData = true;
        }

        return true;
    }

    newData = false;

    uint16_t delayMs = comm_get_send_delay_ms();
    if(motor_setRPM == 0 && nextCommType > 0 &&
       (reg_speed_reference != 0 || reg_input_logic != KRS_INPUT_LOGIC_DISABLED)) {
        nextCommType = 0;
    }

    if(nextCommType > 0) {
        if(controller_is_menu_state(state)) {
            delayMs = comm_get_menu_delay_ms();
        } else if(state == IDLE || state == IDLE_GBW) {
            delayMs = comm_get_idle_delay_ms();
        } else {
            delayMs = comm_get_active_interval_ms();
        }
    }

    if(lastCommSend + delayMs > millis()) {
        return false;
    }

    const int16_t effectiveSetRPM = motor_get_effective_set_rpm();

    if(effectiveSetRPM == 0) {
        if(reg_speed_reference != 0) {
            if(writeSingleRegister(KRS_REG_SPEED_REFERENCE_1, 0)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_input_logic != KRS_INPUT_LOGIC_DISABLED) {
            if(writeSingleRegister(KRS_REG_INPUT_LOGIC_1, KRS_INPUT_LOGIC_DISABLED)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_speed_reference == 0 && reg_input_logic == KRS_INPUT_LOGIC_DISABLED && nextCommType == 0) {
            nextCommType = 1;
        }
    }

    if(effectiveSetRPM != 0) {
        if(reg_control_mode != KRS_SPEED_MODE) {
            if(writeSingleRegister(KRS_REG_CONTROL_MODE, KRS_SPEED_MODE)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_speed_source != KRS_SPEED_SOURCE_INTERNAL_1_TO_8) {
            if(writeSingleRegister(KRS_REG_SPEED_SOURCE, KRS_SPEED_SOURCE_INTERNAL_1_TO_8)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_input_control != KRS_COMM_CONTROL_SON_AND_SP) {
            if(writeSingleRegister(KRS_REG_INPUT_CONTROL_1, KRS_COMM_CONTROL_SON_AND_SP)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_torque_limit_ccw != configuredTorque) {
            if(writeSingleRegister(KRS_REG_TORQUE_LIMIT_CCW, configuredTorque)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_torque_limit_cw != configuredReverseTorque) {
            if(writeSingleRegister(KRS_REG_TORQUE_LIMIT_CW, configuredReverseTorque)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_speed_reference != effectiveSetRPM) {
            if(writeSingleRegister(KRS_REG_SPEED_REFERENCE_1, static_cast<uint16_t>(effectiveSetRPM))) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }
        if(reg_input_logic != KRS_INPUT_LOGIC_ENABLED_INTERNAL_SPEED_1) {
            if(writeSingleRegister(KRS_REG_INPUT_LOGIC_1, KRS_INPUT_LOGIC_ENABLED_INTERNAL_SPEED_1)) {
                lastCommSend = millis();
                nextCommType = 0;
                motor_lastSetRPM = motor_setRPM;
                return false;
            }
        }

        if(reg_control_mode == KRS_SPEED_MODE &&
           reg_speed_source == KRS_SPEED_SOURCE_INTERNAL_1_TO_8 &&
           reg_input_control == KRS_COMM_CONTROL_SON_AND_SP &&
           reg_torque_limit_ccw == configuredTorque &&
           reg_torque_limit_cw == configuredReverseTorque &&
           reg_speed_reference == effectiveSetRPM &&
           reg_input_logic == KRS_INPUT_LOGIC_ENABLED_INTERNAL_SPEED_1 &&
           nextCommType == 0) {
            nextCommType = 1;
        }
    }

    if(nextCommType > 0 && nextCommType < 10) {
        readRegister(KRS_REG_MONITOR_DN01, KRS_MONITOR_COUNT);
        lastCommSend = millis();
    }

    return false;
}

bool motor_krs_130st_m07725_off_backend() {
    uint8_t score = 0;
    for(int i = 0; i < 10; i++) {
        if(writeConfirm(KRS_REG_SPEED_REFERENCE_1, 0)) {
            reg_speed_reference = 0;
            score++;
            break;
        }
        delay(200);
    }
    for(int i = 0; i < 10; i++) {
        delay(200);
        if(writeConfirm(KRS_REG_INPUT_LOGIC_1, KRS_INPUT_LOGIC_DISABLED)) {
            reg_input_logic = KRS_INPUT_LOGIC_DISABLED;
            score++;
            break;
        }
    }
    return score == 2;
}

void motor_krs_130st_m07725_sleep_backend() {
    motor_krs_130st_m07725_off_backend();
}

void motor_krs_130st_m07725_reset_backend() {
    motor_krs_130st_m07725_off_backend();
}

bool motor_krs_130st_m07725_calibrate_backend() {
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

    if(currentCal < (SIZEOFCALIBRATEARRAY / sizeof(uint16_t)) && checkTime + CAL_TIME < millis() && sumCount != 0) {
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

const MotorBackendDefinition MOTOR_BACKEND_KRS_130ST_M07725 = {
    "krs130st-m07725",
    "KRS 130ST-M07725 servo backend",
    "KRS Modbus RTU speed-mode backend for drives whose Pn001 motor code is 20 (130ST-M07725).",
    "Probe Pn001 at 0x0001 over the KRS default 9600 8O1 Modbus RTU profile and claim only when it equals motor code 20.",
    "Use Pn002 speed mode, Pn168 internal speed source, Pn169 signed internal speed 1, Pn068 communication-controlled SON/SP inputs, Pn070 SON/SP state, and Pn008/Pn009 torque limits.",
    "Read Dn001..Dn008 from 0x0171; Dn002 is average torque percent and Dn008 is current motor speed in rpm.",
    {
        motor_krs_130st_m07725_probe_backend,
        motor_krs_130st_m07725_init_backend,
        motor_krs_130st_m07725_do_comm_backend,
        motor_krs_130st_m07725_off_backend,
        motor_krs_130st_m07725_calibrate_backend,
        motor_krs_130st_m07725_sleep_backend,
        motor_krs_130st_m07725_reset_backend,
    },
};
