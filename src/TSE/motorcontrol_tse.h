#pragma once
#include "motor_backend.h"
#include "kfm_zero_probe.h"
#include "kfm_pa25_probe.h"
#include "kfm_pa25_pulse.h"
struct TseStartupSnapshot {
    bool captured;
    bool beforeAnyBackendWrite;
    uint16_t validMask;
    uint32_t startedAtMs;
    uint32_t completedAtMs;
    uint16_t speedSource;
    uint16_t speedLimit;
    int16_t speedCommand;
    int16_t zeroPreset;
    uint16_t softwareEnable;
    uint16_t forcedInputs[5];
    int16_t actualRpm;
};
struct TseDiagnostics {
    TseStartupSnapshot startup;
    const char* phase;
    const char* reason;
    uint16_t pendingRegister;
    uint16_t rejectedRegister;
    uint16_t actual;
    uint16_t expected;
    uint32_t successfulReads;
    uint32_t successfulWrites;
    uint32_t timeouts;
    uint32_t modbusExceptions;
    uint32_t diagnosticSessions;
    uint32_t cancelledPollReads;
    bool pending;
    int16_t commandRpm;
    int8_t primaryDirectionSign;
    int16_t torqueRaw;
    bool torqueScalingVerified;
    bool torqueRawValid;
    uint32_t torqueRawAtMs;
    bool identified;
    bool kfm;
    bool motionArmed;
    uint32_t unsentWriteAttempts;
    uint8_t zeroEnableReadbackMask;
    uint8_t stopReadbackMask;
    bool runCandidate;
    bool stopReadbackVerified;
    bool monitorValid;
    bool alarmValid;
};
const TseDiagnostics& motor_tse_diagnostics();
bool motor_tse_zero_probe_available();
bool motor_tse_zero_probe_start();
bool motor_tse_zero_probe_cancel();
bool motor_tse_zero_probe_write_window();
const kfm_zero_probe::View& motor_tse_zero_probe_view();
bool motor_tse_begin_diagnostic_read();
bool motor_tse_store_zero_default();
bool motor_tse_arm();
bool motor_tse_request_user_start();
bool motor_tse_acknowledge_stop();
bool motor_tse_stop_ack_required();
void motor_tse_disarm();
extern const MotorBackendDefinition MOTOR_BACKEND_TSE;

uint8_t motor_tse_commissioning_write_windows();
bool motor_tse_pa25_probe_available();
bool motor_tse_pa25_probe_start();
bool motor_tse_pa25_probe_cancel();
const kfm_pa25_probe::View& motor_tse_pa25_probe_view();

bool motor_tse_pa25_probe_recover();

bool motor_tse_pa25_pulse_available();
bool motor_tse_pa25_pulse_start();
bool motor_tse_pa25_pulse_cancel();
bool motor_tse_pa25_pulse_recover();
const kfm_pa25_pulse::View& motor_tse_pa25_pulse_view();

struct TseTouchTestView {
 bool available=false,armed=false,runActive=false,remoteFresh=false;
 const char* reason="unavailable";
 uint32_t armedAtMs=0;
};
TseTouchTestView motor_tse_touch_test_view();
bool motor_tse_touch_test_arm();
bool motor_tse_touch_test_disarm();
bool motor_tse_touch_test_request_start();
bool motor_tse_touch_test_request_stop();
