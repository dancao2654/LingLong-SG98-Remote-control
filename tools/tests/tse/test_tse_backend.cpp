#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

#include "TSE/motorcontrol_tse.h"
#include "main.h"
#include "motor_hal.h"

uint16_t lastRead[16] = {};
uint8_t lastRequestType = 0;
int16_t motor_setRPM = 0;
int16_t motor_currentRPM = 0;
int16_t motor_currentTorque = 0;
int16_t commCounter = 0;
COMM_STATUS commStatus = COMM_DISCONNECTED;
SYSTEM_STATUS currentStatus = MOTOR_NOT_CONNECTED;
States state = IDLE;
uint8_t error = 0;
uint16_t errorCode = 0;
bool newData = false;
bool disp_updateRequired = false;

namespace {
struct Write { uint16_t reg; uint16_t value; };
std::vector<Write> writes;
std::map<uint16_t, uint16_t> registers;
uint32_t now = 0;
bool linkUp = true;
bool responseWaiting = false;
bool responseWrite = false;
bool corruptAck = false;
bool corruptCount = false;
uint16_t responseReg = 0;
uint16_t responseCount = 0;
uint16_t responseValue = 0;
int8_t direction = 1;
int16_t forwardTorque = 100;
int16_t reverseTorque = 35;
const auto& api = MOTOR_BACKEND_TSE.api;

void recordWrite(uint16_t reg, uint16_t value) {
    writes.push_back({reg, value});
    if(linkUp && reg >= 0x80 && reg < 0x100) registers[reg - 0x80] = value;
}

void fixture() {
    registers.clear();
    registers[1] = 80;
    registers[2] = 123;
    registers[62] = 4;
    registers[71] = 1;
    registers[72] = 96;
    registers[73] = 0;
    registers[0x0101] = 2;
    registers[0x0102] = 3;
    registers[0x0103] = 4;
    writes.clear();
    now = 0;
    linkUp = true;
    responseWaiting = false;
    corruptAck = false;
    corruptCount = false;
    direction = 1;
    forwardTorque = 100;
    reverseTorque = 35;
    error = 0;
    errorCode = 0;
    state = IDLE;
    api.init();
}

void pump(int count = 1) {
    for(int i = 0; i < count; ++i) {
        now += 25;
        api.tick();
    }
}

void ready() {
    for(int i = 0; i < 200 && currentStatus != MOTOR_READY; ++i) pump();
    assert(currentStatus == MOTOR_READY);
}

bool wrote(uint16_t reg, int16_t value) {
    return std::any_of(writes.begin(), writes.end(), [=](const Write& write) {
        return write.reg == reg && write.value == static_cast<uint16_t>(value);
    });
}

size_t indexOf(uint16_t reg, int16_t value) {
    for(size_t i = 0; i < writes.size(); ++i) {
        if(writes[i].reg == reg && writes[i].value == static_cast<uint16_t>(value)) return i;
    }
    return writes.size();
}

void runAt(int16_t rpm) {
    state = GRINDING;
    motor_setRPM = rpm;
    for(int i = 0; i < 100 && currentStatus != MOTOR_ENABLED; ++i) pump();
    assert(currentStatus == MOTOR_ENABLED);
}
}

uint32_t millis() { return now; }
bool controller_is_menu_state(States value) { return value == MENU1; }
uint16_t comm_get_active_interval_ms() { return 60; }
int16_t motor_get_effective_set_rpm() { return motor_setRPM * direction; }
int16_t motor_get_formatted_command_value(MotorCommandId, int8_t sign) {
    return sign > 0 ? forwardTorque : -reverseTorque;
}
bool motor_probe_set_profile(CommProfile) { return true; }
bool motor_probe_read_register(uint16_t reg, uint16_t count, uint8_t,
                               uint16_t* values, uint16_t maxValues) {
    if(!linkUp) return false;
    assert(count <= maxValues);
    for(uint16_t i = 0; i < count; ++i) values[i] = registers[reg + i];
    return true;
}
bool readRegister(uint16_t reg, uint16_t count) {
    assert(count <= 16);
    responseWaiting = linkUp;
    responseWrite = false;
    responseReg = reg;
    responseCount = count;
    return true;
}
bool writeSingleRegister(uint16_t reg, uint16_t value) {
    recordWrite(reg, value);
    responseWaiting = linkUp;
    responseWrite = true;
    responseReg = reg;
    responseValue = value;
    return true;
}
bool writeConfirm(uint16_t reg, uint16_t value) {
    recordWrite(reg, value);
    responseWaiting = false;
    return linkUp;
}
int16_t receive() {
    if(!responseWaiting || !linkUp) return 0;
    responseWaiting = false;
    if(responseWrite) {
        lastRequestType = 0;
        lastRead[0] = corruptAck ? responseValue + 1 : responseValue;
    } else {
        lastRequestType = 100 + responseCount - (corruptCount ? 1 : 0);
        for(uint16_t i = 0; i < responseCount; ++i) lastRead[i] = registers[responseReg + i];
        if(responseReg == 0x1000) lastRead[9] = registers[4];
    }
    return responseReg;
}

int main() {
    fixture();
    MotorProbeReport report = {};
    assert(api.probe(&report) && report.tse_signature);
    assert(writes.empty());
    for(uint16_t reg : {1, 2, 62, 71, 72, 73}) {
        const uint16_t saved = registers[reg];
        registers[reg] = reg == 73 ? 1 : 0;
        assert(!api.probe(&report));
        registers[reg] = saved;
    }
    assert(writes.empty());

    fixture();
    registers[1] = 20; // A KRS model code is not a TSE fingerprint.
    pump(80);
    assert(writes.empty());
    assert(!api.off());
    assert(writes.empty());

    fixture();
    registers[0x0100] = 1; // Default physical SON must be unassigned first.
    pump(100);
    assert(currentStatus == MOTOR_NOT_READY && error == 4);
    assert(!wrote(0x00B5, 1));
    registers[0x0100] = 0;
    ready();
    assert(error == 0);

    for(uint16_t reg : {0x010F, 0x0110, 0x0111, 0x0112, 0x0113, 0x011E, 0x011F}) {
        fixture();
        registers[reg] = 1;
        pump(100);
        assert(currentStatus == MOTOR_NOT_READY);
        assert(!wrote(0x00B5, 1));
    }

    fixture();
    ready();
    assert(indexOf(0x0098, 0) < indexOf(0x0084, 1));
    assert(indexOf(0x00B5, 0) < indexOf(0x0084, 1));
    assert(wrote(0x0096, 1) && wrote(0x0097, 3000) && wrote(0x00CA, 1));
    writes.clear();
    runAt(700);
    assert(indexOf(0x00A2, 100) < indexOf(0x00B5, 1));
    assert(indexOf(0x00A3, -35) < indexOf(0x00B5, 1));
    assert(indexOf(0x0098, 700) < indexOf(0x00B5, 1));
    registers[0x1000] = static_cast<uint16_t>(-600);
    registers[0x1007] = static_cast<uint16_t>(-30);
    pump(12);
    assert(motor_currentRPM == -600 && motor_currentTorque == 30);
    writes.clear();
    motor_setRPM = 0;
    pump(12);
    assert(indexOf(0x0098, 0) < indexOf(0x00B5, 0));
    assert(currentStatus == MOTOR_READY);

    fixture();
    ready();
    direction = -1;
    writes.clear();
    runAt(4000);
    assert(wrote(0x0098, -3000));
    for(const auto& write : writes) assert(write.reg >= 0x0080 && write.reg < 0x0100);
    writes.clear();
    assert(api.off());
    assert(writes.size() == 2 && writes[0].reg == 0x0098 && writes[1].reg == 0x00B5);

    fixture();
    ready();
    runAt(700);
    registers[0x1013] = 7;
    pump(20);
    assert(motor_setRPM == 0 && error == 3 && errorCode == 7);
    assert(currentStatus == MOTOR_FAULT);
    assert(registers[24] == 0 && registers[53] == 0);

    fixture();
    ready();
    runAt(700);
    writes.clear();
    linkUp = false;
    pump(20);
    assert(motor_setRPM == 0 && commStatus == COMM_DISCONNECTED);
    assert(wrote(0x0098, 0) && wrote(0x00B5, 0));
    linkUp = true;
    writes.clear();
    ready();
    pump(20);
    assert(motor_setRPM == 0 && !wrote(0x00B5, 1));
    runAt(500); // A fresh Start can run after recovery.

    fixture();
    ready();
    registers[4] = 0; // A drive-only reboot/mode reset cancels the run.
    motor_setRPM = 700;
    pump(20);
    assert(motor_setRPM == 0);

    fixture();
    ready();
    runAt(700);
    registers[24] = 0; // Reboot is detected even when persistent mode is 1.
    registers[53] = 0;
    pump(30);
    assert(motor_setRPM == 0);

    fixture();
    ready();
    runAt(700);
    registers[0x1013] = 7;
    for(int i = 0; i < 30 && error != 3; ++i) {
        motor_setRPM = 800 + i;
        pump();
    }
    assert(error == 3); // Command churn cannot starve alarm monitoring.

    fixture();
    now = UINT32_MAX - 300;
    ready();
    runAt(700);
    assert(api.off());

    fixture();
    corruptAck = true;
    pump(20);
    assert(currentStatus != MOTOR_READY && !wrote(0x00B5, 1));
    fixture();
    corruptCount = true;
    pump(40);
    assert(currentStatus != MOTOR_READY && !wrote(0x00B5, 1));

    fixture();
    ready();
    runAt(700);
    linkUp = false;
    writes.clear();
    assert(!api.off());
    assert(wrote(0x0098, 0) && wrote(0x00B5, 0));

    fixture();
    ready();
    runAt(700);
    pump(); // Leave telemetry pending and then lose the response.
    linkUp = false;
    motor_setRPM = 0;
    writes.clear();
    pump();
    assert(wrote(0x0098, 0) && wrote(0x00B5, 0));

    std::puts("TSE backend regression tests passed");
}
