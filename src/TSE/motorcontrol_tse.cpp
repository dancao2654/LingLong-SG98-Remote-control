#include "motorcontrol_tse.h"

#include "display.h"
#include "main.h"
#include "motor_hal.h"
#include "motor_probe.h"

namespace {
// TSD/TSE manual 2024-12-16, sections 7.1, 7.2 and 8.4.
constexpr uint16_t VOLATILE_PA = 0x0080;
constexpr uint16_t SPEED = VOLATILE_PA + 24;
constexpr uint16_t ENABLE = VOLATILE_PA + 53;
constexpr uint16_t TORQUE_CCW = VOLATILE_PA + 34;
constexpr uint16_t TORQUE_CW = VOLATILE_PA + 35;
constexpr uint16_t MONITOR = 0x1000;
constexpr uint16_t ALARM = 0x1013;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 300;
constexpr uint32_t RETRY_MS = 1000;
constexpr int16_t MAX_RPM = 3000;

enum class Setup : uint8_t {
    Identity, StopSpeed, StopEnable, Inputs, ForcedInputs, VirtualInputs,
    Mode, Source, SpeedLimit, CommStop, Complete
};

Setup setup = Setup::Identity;
bool pending = false;
bool pendingWrite = false;
uint16_t pendingReg = 0;
uint16_t pendingValue = 0;
uint16_t pendingCount = 0;
uint32_t sentAt = 0;
uint32_t nextAttemptAt = 0;
uint32_t lastPollAt = 0;
bool retryWaiting = false;
bool monitorValid = false;
bool alarmValid = false;
uint8_t pollStep = 0;
bool identified = false;
int16_t cachedSpeed = 0;
int16_t cachedEnable = 0;
int16_t cachedCcw = -1;
int16_t cachedCw = 1;
uint16_t driveAlarm = 0;

bool probe(MotorProbeReport* report) {
    if(report == nullptr) return false;
    report->tse_signature = false;
    motor_probe_set_profile(COMM_PROFILE_TSE);
    uint16_t model[2] = {};
    uint16_t comm[3] = {};
    uint16_t encoder[1] = {};
    // No write-based identification, including when the family is forced.
    const bool matched =
        motor_probe_read_register(1, 2, 1, model, 2) &&
        model[0] >= 40 && model[0] <= 80 && model[1] != 0 && model[1] != 0xFFFF &&
        motor_probe_read_register(71, 3, 1, comm, 3) &&
        comm[0] == SLAVE_ID && comm[1] == 96 && comm[2] == 0 &&
        motor_probe_read_register(62, 1, 1, encoder, 1) &&
        (encoder[0] == 4 || encoder[0] == 5);
    report->tse_signature = matched;
    return matched;
}

void cancelRun(uint8_t code) {
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    if(!controller_is_menu_state(state)) state = IDLE;
    error = code;
    newData = false;
    disp_updateRequired = true;
}

void retrySetup(uint8_t code) {
    cancelRun(code);
    pending = false;
    if(identified) {
        writeConfirm(SPEED, 0);
        writeConfirm(ENABLE, 0);
    }
    identified = false;
    cachedSpeed = 0;
    cachedEnable = 0;
    cachedCcw = -1;
    cachedCw = 1;
    monitorValid = false;
    alarmValid = false;
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
    setup = Setup::Identity;
    nextAttemptAt = millis();
    retryWaiting = true;
}

bool sendRead(uint16_t reg, uint16_t count) {
    if(!readRegister(reg, count)) return false;
    pending = true;
    pendingWrite = false;
    pendingReg = reg;
    pendingCount = count;
    sentAt = millis();
    return true;
}

bool sendWrite(uint16_t reg, int16_t value) {
    if(!writeSingleRegister(reg, static_cast<uint16_t>(value))) return false;
    pending = true;
    pendingWrite = true;
    pendingReg = reg;
    pendingValue = static_cast<uint16_t>(value);
    sentAt = millis();
    return true;
}

void initTse() {
    pending = false;
    retryWaiting = false;
    monitorValid = false;
    alarmValid = false;
    pollStep = 0;
    identified = false;
    cachedSpeed = 0;
    cachedEnable = 0;
    cachedCcw = -1;
    cachedCw = 1;
    driveAlarm = 0;
    setup = Setup::Identity;
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
    newData = false;
}

bool inputsSafe() {
    // Software enable must be the sole enable source. Preserve physical limit
    // and torque-limit inputs; never remap or bypass them automatically.
    for(uint8_t i = 0; i < 4; ++i) {
        const uint16_t function = lastRead[i];
        if(function != 0 && function != 2 && function != 3 && function != 4 &&
           function != 5 && function != 6) return false;
    }
    return true;
}

void setupRejected() {
    cancelRun(4);
    currentStatus = MOTOR_NOT_READY;
    setup = Setup::Inputs;
    nextAttemptAt = millis();
    retryWaiting = true;
}

void acceptResponse() {
    commCounter = 0;
    commStatus = COMM_CONNECTED;
    if(setup != Setup::Complete) {
        if(setup == Setup::StopSpeed) cachedSpeed = 0;
        if(setup == Setup::StopEnable) cachedEnable = 0;
        if(setup == Setup::Inputs && !inputsSafe()) {
            setupRejected();
            return;
        }
        if(setup == Setup::ForcedInputs) {
            for(uint8_t i = 0; i < 5; ++i) {
                if(lastRead[i] != 0) {
                    setupRejected();
                    return;
                }
            }
        }
        if(setup == Setup::VirtualInputs && (lastRead[0] != 0 || lastRead[1] != 0)) {
            setupRejected();
            return;
        }
        setup = static_cast<Setup>(static_cast<uint8_t>(setup) + 1);
        currentStatus = MOTOR_NOT_READY;
        return;
    }

    if(pendingWrite) {
        const int16_t value = static_cast<int16_t>(pendingValue);
        if(pendingReg == SPEED) cachedSpeed = value;
        if(pendingReg == ENABLE) cachedEnable = value;
        if(pendingReg == TORQUE_CCW) cachedCcw = value;
        if(pendingReg == TORQUE_CW) cachedCw = value;
    } else if(pendingReg == ALARM) {
        driveAlarm = lastRead[0];
        alarmValid = true;
        pollStep = 1;
        if(driveAlarm != 0) {
            errorCode = driveAlarm;
            cancelRun(3);
        }
    } else if(pendingReg == MONITOR) {
        motor_currentRPM = static_cast<int16_t>(lastRead[0]);
        int32_t torque = static_cast<int16_t>(lastRead[7]);
        if(torque < 0) torque = -torque;
        // Shared purge learning expects load magnitude in either direction.
        motor_currentTorque = static_cast<int16_t>(torque > 32767 ? 32767 : torque);
        monitorValid = true;
        pollStep = 2;
        if(lastRead[9] != 1) {
            retrySetup(4);
            return;
        }
        newData = true;
    } else if(pendingReg == 22) {
        pollStep = 3;
        if(lastRead[0] != 1 || lastRead[1] != MAX_RPM ||
           static_cast<int16_t>(lastRead[2]) != cachedSpeed) {
            retrySetup(4);
            return;
        }
    } else if(pendingReg == 53) {
        pollStep = 0;
        if(lastRead[0] != cachedEnable) {
            retrySetup(4);
            return;
        }
    }
    if(driveAlarm != 0) currentStatus = MOTOR_FAULT;
    else if(!monitorValid || !alarmValid) currentStatus = MOTOR_NOT_READY;
    else {
        currentStatus = cachedEnable == 1 ? MOTOR_ENABLED : MOTOR_READY;
        if(error == 1 || error == 2 || error == 4) error = 0;
    }
}

bool sendPoll() {
    const uint16_t registers[] = {ALARM, MONITOR, 22, 53};
    const uint16_t counts[] = {1, 10, 3, 1};
    if(!sendRead(registers[pollStep], counts[pollStep])) return false;
    lastPollAt = millis();
    return true;
}

bool off();

bool tick() {
    newData = false;
    if(pending) {
        // A user stop must not wait for a telemetry or run acknowledgement.
        if(setup == Setup::Complete && motor_setRPM == 0 &&
           (cachedSpeed != 0 || cachedEnable != 0 ||
            (pendingWrite && (pendingReg == SPEED || pendingReg == ENABLE) && pendingValue != 0))) {
            return off();
        }
        const int16_t received = receive();
        const bool expectedType = pendingWrite ? lastRequestType == 0 :
            lastRequestType == 100 + pendingCount;
        if(received == pendingReg && expectedType &&
           (!pendingWrite || lastRead[0] == pendingValue)) {
            pending = false;
            acceptResponse();
            return true;
        }
        if(static_cast<uint32_t>(millis() - sentAt) >= RESPONSE_TIMEOUT_MS) {
            retrySetup(2);
        }
        return false;
    }
    if(retryWaiting) {
        motor_setRPM = 0;
        if(static_cast<uint32_t>(millis() - nextAttemptAt) < RETRY_MS) return false;
        retryWaiting = false;
    }
    if(setup != Setup::Complete) {
        motor_setRPM = 0;
        switch(setup) {
            case Setup::Identity: {
                MotorProbeReport report = {};
                if(!probe(&report)) retrySetup(1);
                else {
                    identified = true;
                    setup = Setup::StopSpeed;
                }
                break;
            }
            case Setup::StopSpeed: sendWrite(SPEED, 0); break;
            case Setup::StopEnable: sendWrite(ENABLE, 0); break;
            case Setup::Inputs: sendRead(0x0100, 4); break;
            case Setup::ForcedInputs: sendRead(0x010F, 5); break;
            case Setup::VirtualInputs: sendRead(0x011E, 2); break;
            case Setup::Mode: sendWrite(VOLATILE_PA + 4, 1); break;
            case Setup::Source: sendWrite(VOLATILE_PA + 22, 1); break;
            case Setup::SpeedLimit: sendWrite(VOLATILE_PA + 23, MAX_RPM); break;
            case Setup::CommStop: sendWrite(VOLATILE_PA + 74, 1); break;
            case Setup::Complete: break;
        }
        return false;
    }

    if(!monitorValid || !alarmValid || driveAlarm != 0) motor_setRPM = 0;
    int16_t target = motor_get_effective_set_rpm();
    if(target > MAX_RPM) target = MAX_RPM;
    if(target < -MAX_RPM) target = -MAX_RPM;
    if(target == 0) {
        if(cachedSpeed != 0) { sendWrite(SPEED, 0); return false; }
        if(cachedEnable != 0) { sendWrite(ENABLE, 0); return false; }
    } else {
        // Continuous speed/menu changes must not starve fault/reboot monitoring.
        if(static_cast<uint32_t>(millis() - lastPollAt) >= 150) {
            sendPoll();
            return false;
        }
        int16_t ccw = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, 1);
        int16_t cw = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, -1);
        if(ccw < 0) ccw = 0;
        if(ccw > 300) ccw = 300;
        if(cw > 0) cw = 0;
        if(cw < -300) cw = -300;
        if(cachedCcw != ccw) { sendWrite(TORQUE_CCW, ccw); return false; }
        if(cachedCw != cw) { sendWrite(TORQUE_CW, cw); return false; }
        if(cachedSpeed != target) { sendWrite(SPEED, target); return false; }
        if(cachedEnable != 1) { sendWrite(ENABLE, 1); return false; }
    }
    if(static_cast<uint32_t>(millis() - lastPollAt) >= comm_get_active_interval_ms()) {
        sendPoll();
    }
    return false;
}

bool off() {
    motor_setRPM = 0;
    pending = false;
    if(!identified) return false;
    // Both commands are attempted even if the first acknowledgement is lost.
    const bool speedStopped = writeConfirm(SPEED, 0);
    const bool disabled = writeConfirm(ENABLE, 0);
    if(speedStopped) cachedSpeed = 0;
    if(disabled) cachedEnable = 0;
    if(!speedStopped || !disabled) retrySetup(2);
    else if(driveAlarm == 0) currentStatus =
        setup == Setup::Complete && monitorValid && alarmValid ? MOTOR_READY : MOTOR_NOT_READY;
    return speedStopped && disabled;
}

void sleepTse() { off(); }
void resetTse() {
    off();
    initTse();
}
}

const MotorBackendDefinition MOTOR_BACKEND_TSE = {
    "tse", "TSE servo backend",
    "TSD/TSE family speed-mode backend; KFM variant follows the family manual, not hardware-validated.",
    "Read-only fingerprint: PA1 model 40..80, PA2 software, PA62 encoder 4/5, PA71/72/73 = 1/96/0 (9600 8N2).",
    "PA4=1, PA22=1, volatile PA24 speed, PA34/35 signed torque limits, PA53 software enable. Physical SON/SP inputs must be unassigned; see TSE setup guide.",
    "0x1000 signed rpm, 0x1007 torque percent (reported as magnitude), 0x1009 mode, 0x1013 fault. No automatic alarm reset.",
    {probe, initTse, tick, off, nullptr, sleepTse, resetTse},
};
