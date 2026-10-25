#pragma once
#include "comm.h"

struct MotorProbeReport;
struct MotorBackendApi {
    bool (*probe)(MotorProbeReport*);
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
