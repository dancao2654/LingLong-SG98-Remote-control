#pragma once

#include "comm.h"

struct MotorProbeReport;

// Standard backend contract for RS-485 motor families.
//
// New motor bring-up should mostly follow this path:
// 1. Clone an existing backend .h/.cpp pair.
// 2. Fill the backend metadata below from the hardware manual/programmer guide.
// 3. Implement the standard API hooks.
// 4. Register the backend in the motor descriptor table with the commands it supports.

struct MotorBackendApi {
    bool (*probe)(MotorProbeReport* report);
    void (*init)();
    bool (*tick)();
    bool (*off)();
    bool (*calibrate)();
    void (*sleep)();
    void (*reset)();
};

struct MotorBackendDefinition {
    const char* key;
    const char* familyLabel;
    const char* description;
    const char* probeGuidance;
    const char* commandGuidance;
    const char* telemetryGuidance;
    MotorBackendApi api;
};
