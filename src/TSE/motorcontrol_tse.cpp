#include "motorcontrol_tse.h"
#include "TSE/kfm_commissioning_policy.h"

#include "display.h"
#include "main.h"
#include "motor_hal.h"
#include "motor_probe.h"
#if (defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST) || (defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE)
#include "espnow_control.h"
#endif

namespace {
// TSD/TSE manual 2024-12-16, sections 7.1, 7.2 and 8.4.
constexpr uint16_t VOLATILE_PA = 0x0080;
constexpr uint16_t SPEED = VOLATILE_PA + 24;
constexpr uint16_t FAMILY_ENABLE = VOLATILE_PA + 53;
constexpr uint16_t KFM_SON = 0x010F;
constexpr uint16_t KFM_SP1 = 0x0110;
// Commissioning limit; do not infer rated motor speed from this value.
constexpr int16_t KFM_MAX_RPM = kfm_commissioning::maxRpm;
constexpr uint16_t TORQUE_CCW = VOLATILE_PA + 34;
constexpr uint16_t TORQUE_CW = VOLATILE_PA + 35;
constexpr uint16_t MONITOR = 0x1000;
constexpr uint16_t ALARM = 0x1013;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 300;
constexpr uint32_t RETRY_MS = 1000;
constexpr int16_t MAX_RPM = 3000;

enum class Setup : uint8_t {
    Identity, StopSpeed, StopEnable, Inputs, ForcedInputs, VirtualInputs,
    Mode, Source, SpeedLimit, CommStop, StopSelect, Complete, AuditSpeed, AuditEnable, AuditForced, AuditRpm
};

Setup setupPhase = Setup::Identity;
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
bool kfm = false;
bool motionArmed = false;
bool recoveryStopRequired = false;
uint16_t checkedSpeedLimit = 0;
uint16_t cachedSelect = 2;
TseDiagnostics diagnostics = {};
bool standaloneStopAckRequired = false;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
bool runWriteWindow = false;
bool candidateEverWritten = false;
uint8_t stopReadbackMask = 0;
uint8_t zeroEnableReadbackMask = 0;
bool reversing = false;
bool runClockActive = false;
uint32_t runStartedAt = 0;
uint32_t armedAt = 0;
bool candidateRemoteFresh() {
    const uint32_t last = espnow_control_last_command_rx_at();
    return espnow_control_is_active() && last != 0 && uint32_t(millis() - last) <= 3000;
}
#endif
uint16_t speedRegister() {
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(kfm) return VOLATILE_PA + 25;
#endif
    return SPEED;
}
uint16_t enableRegister() { return kfm ? KFM_SON : FAMILY_ENABLE; }
const char* phaseName() {
    const char* names[] = {"identity", "stop_speed", "stop_enable", "inputs", "forced_inputs",
                          "virtual_inputs", "mode", "source", "speed_limit", "comm_stop",
                          "stop_select", "complete", "audit_speed", "audit_enable", "audit_forced", "audit_rpm"};
    return names[static_cast<unsigned>(setupPhase)];
}
void rejection(const char* reason, uint16_t reg, uint16_t actual, uint16_t expected) {
    diagnostics.reason = reason;
    diagnostics.rejectedRegister = reg;
    diagnostics.actual = actual;
    diagnostics.expected = expected;
}
void noteBackendWrite() {
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    stopReadbackMask = 0; zeroEnableReadbackMask = 0; candidateEverWritten = true;
#endif
    if(kfm && !diagnostics.startup.captured) diagnostics.startup.beforeAnyBackendWrite = false;
}
bool confirmBackendWrite(uint16_t reg, uint16_t value) {
    noteBackendWrite();
    #if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    runWriteWindow = true;
#endif
    const bool acknowledged = writeConfirm(reg, value);
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    runWriteWindow = false;
#endif
    if(acknowledged) ++diagnostics.successfulWrites;
    return acknowledged;
}
int16_t cachedSpeed = 0;
int16_t cachedEnable = 0;
int16_t cachedCcw = -1;
int16_t cachedCw = 1;
uint16_t driveAlarm = 0;
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
kfm_zero_probe::Probe zeroProbe;
bool zeroProbeSonWindow = false;
bool zeroProbeRead(uint16_t reg, uint16_t count, uint16_t* words) {
    const bool ok = motor_probe_read_register(reg, count, 1, words, count);
    if(ok) ++diagnostics.successfulReads;
    return ok;
}
bool zeroProbeWrite(uint16_t reg, uint16_t value) {
    if(reg == KFM_SON && value == 1) {
        zeroProbeSonWindow = true;
        const bool ok = confirmBackendWrite(reg, value);
        zeroProbeSonWindow = false;
        return ok;
    }
    if((reg == SPEED && value == 0) || (reg == KFM_SON && value == 0) ||
       (reg == KFM_SP1 && value == 2)) return confirmBackendWrite(reg, value);
    return false;
}
const kfm_zero_probe::Io zeroProbeIo = {zeroProbeRead, zeroProbeWrite};
#endif


#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
kfm_pa25_probe::Probe pa25Probe;
bool pa25WriteWindow=false;
bool pa25Read(uint16_t reg,uint16_t count,uint16_t* words){const bool ok=motor_probe_read_register(reg,count,1,words,count);if(ok)++diagnostics.successfulReads;return ok;}
bool pa25Write(uint16_t reg,uint16_t value){
 if(reg==0x99&&value<=1){pa25WriteWindow=true;const bool ok=confirmBackendWrite(reg,value);pa25WriteWindow=false;return ok;}
 if((reg==KFM_SON&&value==0)||(reg==KFM_SP1&&value==2)||(reg==SPEED&&value==0))return confirmBackendWrite(reg,value);
 return false;
}
const kfm_pa25_probe::Io pa25Io={pa25Read,pa25Write};
#endif


#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
kfm_pa25_pulse::Pulse pa25Pulse;
bool pulseWriteWindow=false;
bool pulseRead(uint16_t reg,uint16_t count,uint16_t* words){const bool ok=motor_probe_read_register(reg,count,1,words,count);if(ok)++diagnostics.successfulReads;return ok;}
bool pulseWrite(uint16_t reg,uint16_t value){
 if((reg==0x99&&(value==0||value==static_cast<uint16_t>(kfm_pa25_pulse::kTargetRpm)))||(reg==KFM_SON&&value==1)){
  pulseWriteWindow=true;const bool ok=confirmBackendWrite(reg,value);pulseWriteWindow=false;return ok;
 }
 if((reg==KFM_SON&&value==0)||(reg==SPEED&&value==0)||(reg==KFM_SP1&&value==2))return confirmBackendWrite(reg,value);
 return false;
}
uint32_t pulseNow(){return millis();}
const kfm_pa25_pulse::Io pulseIo={pulseRead,pulseWrite,pulseNow};
#endif


#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
bool touchArmed=false,touchRunActive=false;
uint32_t touchArmedAt=0;
const char* touchReason="not_armed";
bool touchRemoteFresh(){const uint32_t last=espnow_control_last_command_rx_at();return espnow_control_is_active()&&last!=0&&static_cast<uint32_t>(millis()-last)<=3000;}
#endif


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
        ((model[0] >= 40 && model[0] <= 80 && model[1] != 0 && model[1] != 0xFFFF) ||
         (model[0] == 6 && model[1] == 0x1F5C)) &&
        motor_probe_read_register(71, 3, 1, comm, 3) &&
        comm[0] == SLAVE_ID && comm[1] == 96 && comm[2] == 0 &&
        motor_probe_read_register(62, 1, 1, encoder, 1) &&
        (encoder[0] == 4 || encoder[0] == 5);
    kfm = matched && model[0] == 6 && model[1] == 0x1F5C && encoder[0] == 4;
    report->tse_signature = matched && (model[0] != 6 || kfm);
    return report->tse_signature;
}

void cancelRun(uint8_t code) {
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
    if(motionArmed || state == GRINDING || state == GRINDING_GBW || state == PURGING)
        standaloneStopAckRequired = true;
#endif
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
    touchArmed=false;touchRunActive=false;touchReason="backend_fault_lock";
#endif
    if(kfm) motionArmed = false;
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    if(!controller_is_menu_state(state)) {
        const bool weighingMode = state == IDLE_GBW || state == GRINDING_GBW;
        state = weighingMode ? IDLE_GBW : IDLE;
    }
    error = code;
    newData = false;
    disp_updateRequired = true;
}

void retrySetup(uint8_t code) {
    if(kfm && identified) recoveryStopRequired = true;
    cancelRun(code);
    pending = false;
    if(identified) {
        confirmBackendWrite(speedRegister(), 0);
        confirmBackendWrite(enableRegister(), 0);
        if(kfm) confirmBackendWrite(KFM_SP1, 2);
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
    setupPhase = Setup::Identity;
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
    #if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    runWriteWindow = true;
#endif
    const bool sent = writeSingleRegister(reg, static_cast<uint16_t>(value));
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    runWriteWindow = false;
#endif
    if(!sent) { ++diagnostics.unsentWriteAttempts; return false; }
    // A busy transport has sent nothing: keep the checked zero-enable state
    // until a frame actually leaves. Clearing it on an unsent attempt can
    // repeatedly restart verification and starve the first speed command.
    noteBackendWrite();
    pending = true;
    pendingWrite = true;
    pendingReg = reg;
    pendingValue = static_cast<uint16_t>(value);
    sentAt = millis();
    return true;
}

void initTse() {
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    // Reinitialisation is not a fresh power-on snapshot. Preserve its provenance.
    const TseStartupSnapshot bootSnapshot = diagnostics.startup;
    reversing = false; runClockActive = false; runWriteWindow = false;
#endif
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
    touchArmed=false;touchRunActive=false;touchArmedAt=0;touchReason="not_armed";
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    if(pa25Pulse.active())pa25Pulse.cancel(pulseIo);
    const auto& pulse=pa25Pulse.view();
    if(!pulse.writeAttempted||(pulse.restoreAcknowledged&&pulse.restoreReadbackValid))pa25Pulse=kfm_pa25_pulse::Pulse{};
    pulseWriteWindow=false;
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if(pa25Probe.active()) pa25Probe.cancel(pa25Io);
    // Do not erase an unconfirmed restoration on reinitialisation.
    if(!pa25Probe.view().writeAttempted||(pa25Probe.view().restoreAcknowledged&&pa25Probe.view().restoreReadbackValid))pa25Probe=kfm_pa25_probe::Probe{};
    pa25WriteWindow=false;
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(zeroProbe.active()) zeroProbe.cancel(zeroProbeIo);
    zeroProbe = kfm_zero_probe::Probe{};
    zeroProbeSonWindow = false;
#endif
    recoveryStopRequired = false;
    kfm = false;
    motionArmed = false;
    checkedSpeedLimit = 0;
    cachedSelect = 2;
    diagnostics = {};
    diagnostics.reason = "initializing";
    diagnostics.startup.beforeAnyBackendWrite = true;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(bootSnapshot.captured) diagnostics.startup = bootSnapshot;
    else if(candidateEverWritten) diagnostics.startup.beforeAnyBackendWrite = false;
#endif
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
    setupPhase = Setup::Identity;
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
    newData = false;
}

bool inputsSafe() {
    if(kfm) {
        const uint16_t expected[] = {1, 2, 3, 4};
        for(uint8_t i = 0; i < 4; ++i) {
            if(lastRead[i] != expected[i]) {
                rejection("factory_input_mapping", 0x0100 + i, lastRead[i], expected[i]);
                return false;
            }
        }
        return true;
    }
    // Software enable must be the sole enable source. Preserve physical limit
    // and torque-limit inputs; never remap or bypass them automatically.
    for(uint8_t i = 0; i < 4; ++i) {
        const uint16_t function = lastRead[i];
        if(function != 0 && function != 2 && function != 3 && function != 4 &&
           function != 5 && function != 6) {
            rejection("input_function", 0x0100 + i, function, 0);
            return false;
        }
    }
    return true;
}

void setupRejected() {
    cancelRun(4);
    currentStatus = MOTOR_NOT_READY;
    setupPhase = Setup::Inputs;
    nextAttemptAt = millis();
    retryWaiting = true;
}

void acceptResponse() {
    if(pendingWrite) ++diagnostics.successfulWrites;
    else ++diagnostics.successfulReads;
    commCounter = 0;
    commStatus = COMM_CONNECTED;
    if(kfm && setupPhase >= Setup::AuditSpeed) {
        auto& a = diagnostics.startup;
        switch(setupPhase) {
            case Setup::AuditSpeed:
                a.speedSource = lastRead[0]; a.speedLimit = lastRead[1];
                a.speedCommand = static_cast<int16_t>(lastRead[2]);
                a.zeroPreset = static_cast<int16_t>(lastRead[3]);
                a.validMask |= 1; setupPhase = Setup::AuditEnable; break;
            case Setup::AuditEnable:
                a.softwareEnable = lastRead[0];
                a.validMask |= 2; setupPhase = Setup::AuditForced; break;
            case Setup::AuditForced:
                for(uint8_t i = 0; i < 5; ++i) a.forcedInputs[i] = lastRead[i];
                a.validMask |= 4; setupPhase = Setup::AuditRpm; break;
            case Setup::AuditRpm:
                a.actualRpm = static_cast<int16_t>(lastRead[0]);
                a.validMask |= 8; a.completedAtMs = millis();
                a.captured = a.validMask == 15;
                setupPhase = recoveryStopRequired ? Setup::StopSpeed : Setup::Inputs;
                break;
            default: break;
        }
        return;
    }
    if(setupPhase != Setup::Complete) {
        if(setupPhase == Setup::StopSpeed) cachedSpeed = 0;
        if(setupPhase == Setup::StopEnable) cachedEnable = 0;
        if(setupPhase == Setup::Inputs && !inputsSafe()) {
            setupRejected();
            return;
        }
        if(setupPhase == Setup::ForcedInputs) {
            for(uint8_t i = 0; i < 5; ++i) {
                const uint16_t expected = kfm && i == 1 ? 2 : 0;
                if(lastRead[i] != expected) {
                    rejection("forced_input", 0x010F + i, lastRead[i], expected);
                    setupRejected();
                    return;
                }
            }
        }
        if(setupPhase == Setup::VirtualInputs && (lastRead[0] != 0 || lastRead[1] != 0)) {
            const uint16_t i = lastRead[0] != 0 ? 0 : 1;
            rejection("virtual_input", 0x011E + i, lastRead[i], 0);
            setupRejected();
            return;
        }
        if(kfm) {
            // Keep the factory assignments and PA settings. Only zero-speed and
            // the captured SON/SP1 stop sequence are written after preflight.
            if(setupPhase == Setup::Mode && lastRead[0] != 1) {
                rejection("speed_mode", 4, lastRead[0], 1); setupRejected(); return;
            }
            if(setupPhase == Setup::Source && (lastRead[0] != 1 || lastRead[3] != 0)) {
                const uint16_t i = lastRead[0] != 1 ? 0 : 3;
                rejection("speed_source_or_zero_preset", 22 + i, lastRead[i], i == 0 ? 1 : 0);
                setupRejected(); return;
            }
            if(setupPhase == Setup::SpeedLimit) {
                checkedSpeedLimit = lastRead[0];
                if(checkedSpeedLimit < KFM_MAX_RPM) {
                    rejection("speed_limit_too_low", 23, checkedSpeedLimit, KFM_MAX_RPM);
                    setupRejected(); return;
                }
            }
            if(setupPhase == Setup::CommStop) setupPhase = Setup::StopSpeed;
            else if(setupPhase == Setup::StopSpeed) setupPhase = Setup::StopEnable;
            else if(setupPhase == Setup::StopEnable) setupPhase = Setup::StopSelect;
            else if(setupPhase == Setup::StopSelect) {
                cachedSelect = 2;
                setupPhase = recoveryStopRequired ? Setup::Inputs : Setup::Complete;
                recoveryStopRequired = false;
            }
            else setupPhase = static_cast<Setup>(static_cast<uint8_t>(setupPhase) + 1);
        } else {
            if(setupPhase == Setup::CommStop) setupPhase = Setup::Complete;
            else setupPhase = static_cast<Setup>(static_cast<uint8_t>(setupPhase) + 1);
        }
        if(setupPhase == Setup::Complete) diagnostics.reason = kfm ? "motion_locked" : "ready";
        currentStatus = MOTOR_NOT_READY;
        return;
    }

    if(pendingWrite) {
        const int16_t value = static_cast<int16_t>(pendingValue);
        if(pendingReg == speedRegister()) cachedSpeed = value;
        if(pendingReg == enableRegister()) cachedEnable = value;
        if(kfm && pendingReg == KFM_SP1) cachedSelect = value;
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
        diagnostics.torqueRaw = static_cast<int16_t>(lastRead[7]);
        diagnostics.torqueRawAtMs = millis();
        int32_t torque = diagnostics.torqueRaw;
        if(torque < 0) torque = -torque;
        // Shared purge learning expects load magnitude in either direction.
        motor_currentTorque = kfm ? 0 : static_cast<int16_t>(torque > 32767 ? 32767 : torque);
        monitorValid = true;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
        if(kfm && cachedEnable == 1 && !reversing &&
           ((cachedSpeed < 0 && motor_currentRPM > 5) || (cachedSpeed > 0 && motor_currentRPM < -5))) {
            rejection("candidate_direction_mismatch", MONITOR, lastRead[0], static_cast<uint16_t>(cachedSpeed));
            retrySetup(4); return;
        }
        if(kfm && (motor_currentRPM > kfm_commissioning::overspeedRpm || motor_currentRPM < -kfm_commissioning::overspeedRpm)) {
            rejection("candidate_overspeed", MONITOR, lastRead[0], kfm_commissioning::overspeedRpm);
            retrySetup(4); return;
        }
#endif
        pollStep = 2;
        if(lastRead[9] != 1) {
            rejection("monitor_mode", 0x1009, lastRead[9], 1);
            retrySetup(4);
            return;
        }
        newData = true;
    } else if(pendingReg == 22) {
        pollStep = 3;
        const uint16_t speedIndex = speedRegister() == 0x99 ? 3 : 2;
        if(lastRead[0] != 1 || lastRead[1] != (kfm ? checkedSpeedLimit : MAX_RPM) ||
           static_cast<int16_t>(lastRead[speedIndex]) != cachedSpeed) {
            uint16_t i = lastRead[0] != 1 ? 0 : lastRead[1] != (kfm ? checkedSpeedLimit : MAX_RPM) ? 1 : speedIndex;
            const uint16_t expected[] = {1, static_cast<uint16_t>(kfm ? checkedSpeedLimit : MAX_RPM), static_cast<uint16_t>(cachedSpeed), static_cast<uint16_t>(cachedSpeed)};
            rejection("speed_readback", 22 + i, lastRead[i], expected[i]);
            retrySetup(4);
            return;
        }
    } else if(pendingReg == (kfm ? KFM_SON : 53)) {
        pollStep = 0;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
        if(kfm) {
            pollStep = 4;
            for(uint16_t i=2;i<5;++i) if(lastRead[i] != 0) {
                rejection("forced_input_drift", KFM_SON+i, lastRead[i], 0); retrySetup(4); return;
            }
        }
#endif
        if(lastRead[0] != cachedEnable || (kfm && lastRead[1] != cachedSelect)) {
            const uint16_t i = lastRead[0] != cachedEnable ? 0 : 1;
            rejection("enable_or_select_readback", pendingReg + i, lastRead[i], i == 0 ? cachedEnable : cachedSelect);
            retrySetup(4);
            return;
        }
    }
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(kfm && !pendingWrite && (pendingReg == 0x100F || pendingReg == 0x11E)) {
        const uint16_t count = pendingReg == 0x100F ? 1 : 2;
        for(uint16_t i=0;i<count;++i) if(lastRead[i] != 0) {
            rejection("input_status_drift", pendingReg+i, lastRead[i], 0); retrySetup(4); return;
        }
        pollStep = pendingReg == 0x100F ? 5 : 0;
    }
    if(kfm && !pendingWrite && cachedSpeed == 0 && cachedEnable == 1 && cachedSelect == 2) {
        if(pendingReg == MONITOR && motor_currentRPM == 0) zeroEnableReadbackMask |= 1;
        if(pendingReg == 22) zeroEnableReadbackMask |= 2;
        if(pendingReg == KFM_SON) zeroEnableReadbackMask |= 4;
        if(pendingReg == ALARM && driveAlarm == 0) zeroEnableReadbackMask |= 8;
        if(pendingReg == 0x100F) zeroEnableReadbackMask |= 16;
        if(pendingReg == 0x11E) zeroEnableReadbackMask |= 32;
    }
    if(kfm && !pendingWrite && cachedSpeed == 0 && cachedEnable == 0 && cachedSelect == 2) {
        if(pendingReg == MONITOR && motor_currentRPM == 0) stopReadbackMask |= 1;
        if(pendingReg == 22) stopReadbackMask |= 2;
        if(pendingReg == KFM_SON) stopReadbackMask |= 4;
        if(pendingReg == ALARM && driveAlarm == 0) stopReadbackMask |= 8;
        if(pendingReg == 0x100F) stopReadbackMask |= 16;
        if(pendingReg == 0x11E) stopReadbackMask |= 32;
    }
#endif
    if(driveAlarm != 0) currentStatus = MOTOR_FAULT;
    else if(!monitorValid || !alarmValid) currentStatus = MOTOR_NOT_READY;
    else {
        currentStatus = kfm && !motionArmed ? MOTOR_NOT_READY :
            cachedEnable == 1 ? MOTOR_ENABLED : MOTOR_READY;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
        if(kfm && cachedEnable == 0 && (stopReadbackMask != 63 || motor_currentRPM != 0)) currentStatus = MOTOR_NOT_READY;
#endif
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
        if(kfm&&touchArmed&&touchRemoteFresh()&&static_cast<uint32_t>(millis()-touchArmedAt)<30000)currentStatus=MOTOR_READY;
#endif
        if(error == 1 || error == 2 || error == 4) error = 0;
    }
}

bool sendPoll() {
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(kfm) {
        const uint16_t registers[] = {ALARM, MONITOR, 22, KFM_SON, 0x100F, 0x11E};
        const uint16_t counts[] = {1, 10, 4, 5, 1, 2};
        if(!sendRead(registers[pollStep], counts[pollStep])) return false;
        lastPollAt = millis(); return true;
    }
#endif
    const uint16_t registers[] = {ALARM, MONITOR, 22, static_cast<uint16_t>(kfm ? KFM_SON : 53)};
    const uint16_t counts[] = {1, 10, static_cast<uint16_t>(speedRegister() == 0x99 ? 4 : 3), static_cast<uint16_t>(kfm ? 2 : 1)};
    if(!sendRead(registers[pollStep], counts[pollStep])) return false;
    lastPollAt = millis();
    return true;
}

bool off();

bool tick() {
    newData = false;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(kfm && motionArmed) {
        bool finished = runClockActive && (state == IDLE || state == IDLE_GBW);
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
        finished = state == IDLE || state == IDLE_GBW;
        if(finished && (error == 104 || error == 106 || (error > 2 && error < 100)))
            standaloneStopAckRequired = true;
#endif
        if(finished) {
            off(); diagnostics.reason = "operation_finished_disarmed";
        }
        const bool movingRequest = motor_setRPM != 0;
        if(movingRequest && !runClockActive) { runClockActive = true; runStartedAt = millis(); }
        const char* lockReason = !candidateRemoteFresh() ? "remote_link_lost" :
            runClockActive && uint32_t(millis()-runStartedAt) >= kfm_commissioning::maxOperationMs ? "candidate_run_timeout" :
            !runClockActive && uint32_t(millis()-armedAt) >= 30000 ? "candidate_arm_timeout" : nullptr;
        if(lockReason) {
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
            standaloneStopAckRequired = true;
#endif
            const bool weighingMode = state == IDLE_GBW || state == GRINDING_GBW;
            off(); state = weighingMode ? IDLE_GBW : IDLE; diagnostics.reason = lockReason;
        }

    }
#endif
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
    if((touchArmed||touchRunActive)&&!touchRemoteFresh()){
        motor_tse_touch_test_request_stop();touchReason="remote_link_lost";
    }else if(touchArmed&&state!=IDLE){
        touchArmed=false;touchReason="mode_changed_while_armed";currentStatus=MOTOR_NOT_READY;
    }else if(touchArmed&&static_cast<uint32_t>(millis()-touchArmedAt)>=30000){
        touchArmed=false;touchReason="arm_expired";currentStatus=MOTOR_NOT_READY;
    }
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    if(pa25Pulse.active()){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
        if(!touchRunActive||state!=GRINDING){motor_tse_touch_test_request_stop();touchReason="operator_state_stop";}
        motor_setRPM=touchRunActive?100:0;
#else
        if(state!=IDLE||motor_setRPM!=0)pa25Pulse.cancel(pulseIo);
        motor_setRPM=0;
#endif
        pending=false;pa25Pulse.tick(millis(),pulseIo);
        const auto& v=pa25Pulse.view();
        if(v.latestValid)motor_currentRPM=v.latest.rpm;
        motor_currentTorque=0;monitorValid=v.latestValid;alarmValid=v.latestValid;
        currentStatus=MOTOR_NOT_READY;diagnostics.reason=v.reason;
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
        if(pa25Pulse.active())currentStatus=MOTOR_ENABLED;
#endif
        if(!pa25Pulse.active()){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
            touchArmed=false;touchRunActive=false;touchReason=v.reason;motor_setRPM=0;state=IDLE;
#endif
            monitorValid=false;alarmValid=false;pollStep=0;cachedSpeed=0;cachedEnable=0;cachedSelect=2;
            setupPhase=v.phase==kfm_pa25_pulse::Phase::Complete?Setup::Complete:Setup::Identity;
            if(setupPhase==Setup::Identity)identified=false;
            error=v.phase==kfm_pa25_pulse::Phase::Failed?4:0;
        }
        disp_updateRequired=true;return false;
    }
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if(pa25Probe.active()){
        if(state!=IDLE||motor_setRPM!=0)pa25Probe.cancel(pa25Io);
        motor_setRPM=0;pending=false;pa25Probe.tick(millis(),pa25Io);
        const auto& v=pa25Probe.view();
        if(v.hasLatest)motor_currentRPM=v.latest.rpm;
        motor_currentTorque=0;monitorValid=v.latestValid;alarmValid=v.latestValid;
        commStatus=v.failedPolls&&!v.afterGapCaptured?COMM_DISCONNECTED:COMM_CONNECTED;
        currentStatus=MOTOR_NOT_READY;diagnostics.reason=v.reason;
        if(!pa25Probe.active()){
            monitorValid=false;alarmValid=false;pollStep=0;cachedSpeed=0;cachedEnable=0;cachedSelect=2;
            setupPhase=v.phase==kfm_pa25_probe::Phase::Complete?Setup::Complete:Setup::Identity;
            if(setupPhase==Setup::Identity)identified=false;
            error=v.phase==kfm_pa25_probe::Phase::Failed?4:0;
        }
        disp_updateRequired=true;return false;
    }
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(zeroProbe.active()) {
        // Normal run and raw-diagnostic paths cannot interleave with this probe.
        if(state != IDLE || motor_setRPM != 0) zeroProbe.cancel(zeroProbeIo);
        motor_setRPM = 0;
        pending = false;
        zeroProbe.tick(millis(), zeroProbeIo);
        const auto& v = zeroProbe.view();
        if(v.hasLatest) motor_currentRPM = v.latest.rpm;
        motor_currentTorque = 0;
        monitorValid = v.latestValid; alarmValid = v.latestValid;
        commStatus = v.failedPolls && !v.afterGapCaptured ? COMM_DISCONNECTED : COMM_CONNECTED;
        currentStatus = MOTOR_NOT_READY;
        diagnostics.reason = v.reason;
        if(!zeroProbe.active()) {
            monitorValid = false; alarmValid = false; pollStep = 0;
            cachedSpeed = 0; cachedEnable = 0; cachedSelect = 2;
            setupPhase = v.phase == kfm_zero_probe::Phase::Complete ? Setup::Complete : Setup::Identity;
            if(setupPhase == Setup::Identity) identified = false;
            error = v.phase == kfm_zero_probe::Phase::Failed ? 4 : 0;
        }
        disp_updateRequired = true;
        return false;
    }
#endif
    if(pending) {
        // A user stop must not wait for a telemetry or run acknowledgement.
        if(setupPhase == Setup::Complete && motor_setRPM == 0 &&
           (cachedSpeed != 0 || cachedEnable != 0 ||
            (pendingWrite && (pendingReg == speedRegister() || pendingReg == enableRegister()) && pendingValue != 0))) {
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
            // Purge/vibration pauses are part of the same bounded operation.
            const bool keepSession = kfm && motionArmed && (state == GRINDING || state == PURGING || state == GRINDING_GBW);
            const bool stopped = off();
            if(keepSession && stopped) motionArmed = true;
            return stopped;
#else
            return off();
#endif
        }
        const int16_t received = receive();
        if(received == 0x603F) { // TSE never polls that register address.
            ++diagnostics.modbusExceptions;
            rejection("modbus_exception", pendingReg, errorCode, 0);
            pending = false;
            retrySetup(4);
            return false;
        }
        const bool expectedType = pendingWrite ? lastRequestType == 0 :
            lastRequestType == 100 + pendingCount;
        if(received == pendingReg && expectedType &&
           (!pendingWrite || lastRead[0] == pendingValue)) {
            pending = false;
            acceptResponse();
            return true;
        }
        if(static_cast<uint32_t>(millis() - sentAt) >= RESPONSE_TIMEOUT_MS) {
            ++diagnostics.timeouts;
            rejection("response_timeout", pendingReg, 0, pendingCount);
            retrySetup(2);
        }
        return false;
    }
    if(retryWaiting) {
        motor_setRPM = 0;
        if(static_cast<uint32_t>(millis() - nextAttemptAt) < RETRY_MS) return false;
        retryWaiting = false;
    }
    if(setupPhase != Setup::Complete) {
        motor_setRPM = 0;
        switch(setupPhase) {
            case Setup::Identity: {
                MotorProbeReport report = {};
                if(!probe(&report)) { diagnostics.reason = "identity_mismatch_or_no_response"; retrySetup(1); }
                else {
                    identified = true;
                    if(kfm && !diagnostics.startup.captured) {
                        diagnostics.startup.validMask = 0;
                        diagnostics.startup.startedAtMs = millis();
                        setupPhase = Setup::AuditSpeed;
                    } else setupPhase = kfm && !recoveryStopRequired ? Setup::Inputs : Setup::StopSpeed;
                }
                break;
            }
            case Setup::StopSpeed: sendWrite(speedRegister(), 0); break;
            case Setup::StopEnable: sendWrite(enableRegister(), 0); break;
            case Setup::Inputs: sendRead(0x0100, 4); break;
            case Setup::ForcedInputs: sendRead(0x010F, 5); break;
            case Setup::VirtualInputs: sendRead(0x011E, 2); break;
            case Setup::Mode: if(kfm) sendRead(4, 1); else sendWrite(VOLATILE_PA + 4, 1); break;
            case Setup::Source: if(kfm) sendRead(22, 4); else sendWrite(VOLATILE_PA + 22, 1); break;
            case Setup::SpeedLimit: if(kfm) sendRead(23, 1); else sendWrite(VOLATILE_PA + 23, MAX_RPM); break;
            case Setup::CommStop: if(kfm) sendRead(74, 1); else sendWrite(VOLATILE_PA + 74, 1); break;
            case Setup::StopSelect: sendWrite(KFM_SP1, 2); break;
            case Setup::AuditSpeed: sendRead(22, 4); break;
            case Setup::AuditEnable: sendRead(53, 1); break;
            case Setup::AuditForced: sendRead(0x010F, 5); break;
            case Setup::AuditRpm: sendRead(MONITOR, 1); break;
            case Setup::Complete: break;
        }
        return false;
    }

    if(!monitorValid || !alarmValid || driveAlarm != 0 || (kfm && !motionArmed)) motor_setRPM = 0;
    int32_t requested = motor_get_effective_set_rpm();
    // Factory capture S007/S008: normal grinding uses negative PA24, reverse
    // uses positive PA24. Preserve the shared user's normal/reverse setting.
    if(kfm) requested = -requested;
    const int16_t maxRpm = kfm ? KFM_MAX_RPM : MAX_RPM;
    if(requested > maxRpm) requested = maxRpm;
    if(requested < -maxRpm) requested = -maxRpm;
    const int16_t target = static_cast<int16_t>(requested);
    if(target == 0) {
        if(cachedSpeed != 0) { sendWrite(speedRegister(), 0); return false; }
        if(cachedEnable != 0) { sendWrite(enableRegister(), 0); return false; }
        if(kfm && cachedSelect != 2) { sendWrite(KFM_SP1, 2); return false; }
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
        if(!kfm && cachedCcw != ccw) { sendWrite(TORQUE_CCW, ccw); return false; }
        if(!kfm && cachedCw != cw) { sendWrite(TORQUE_CW, cw); return false; }
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
        if(kfm) {
            if(cachedSelect != 2) { retrySetup(4); return false; }
            if((cachedSpeed < 0 && target > 0) || (cachedSpeed > 0 && target < 0)) reversing = true;
            if(reversing) {
                if(cachedSpeed != 0) { sendWrite(speedRegister(), 0); return false; }
                if(cachedEnable != 0) { sendWrite(enableRegister(), 0); return false; }
                if(motor_currentRPM != 0) { sendPoll(); return false; }
                reversing = false;
            }
            // PA25 is already zero at preflight. Enable before the first speed
            // write, exactly as in the hardware-verified pulse sequence.
            if(cachedEnable != 1) {
                if(motor_currentRPM != 0) { sendPoll(); return false; }
                sendWrite(enableRegister(), 1); return false;
            }
            if(cachedSpeed == 0 && (zeroEnableReadbackMask != 63 || motor_currentRPM != 0)) { sendPoll(); return false; }
            if(cachedSpeed != target) { sendWrite(speedRegister(), target); return false; }
        } else
#endif
        {
            if(kfm && cachedSelect != 0) { sendWrite(KFM_SP1, 0); return false; }
            if(cachedSpeed != target) { sendWrite(speedRegister(), target); return false; }
            if(cachedEnable != 1) { sendWrite(enableRegister(), 1); return false; }
        }
    }
    if(static_cast<uint32_t>(millis() - lastPollAt) >= comm_get_active_interval_ms()) {
        sendPoll();
    }
    return false;
}

bool off() {
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
    touchArmed=false;touchRunActive=false;touchReason="disarmed";state=IDLE;
#endif
    bool probeStopped = true;
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    if(pa25Pulse.active()||(pa25Pulse.view().writeAttempted&&(!pa25Pulse.view().restoreAcknowledged||!pa25Pulse.view().restoreReadbackValid)))probeStopped=pa25Pulse.cancel(pulseIo);
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if(pa25Probe.active() || (pa25Probe.view().writeAttempted && (!pa25Probe.view().restoreAcknowledged || !pa25Probe.view().restoreReadbackValid)))
        probeStopped = pa25Probe.cancel(pa25Io);
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(zeroProbe.active()) zeroProbe.cancel(zeroProbeIo);
#endif
    if(kfm) { motionArmed = false; diagnostics.reason = "motion_locked"; }
    motor_setRPM = 0;
    pending = false;
    if(!identified) return false;
    // Both commands are attempted even if the first acknowledgement is lost.
    const bool speedStopped = confirmBackendWrite(speedRegister(), 0);
    const bool disabled = confirmBackendWrite(enableRegister(), 0);
    const bool selectedStop = !kfm || confirmBackendWrite(KFM_SP1, 2);
    if(selectedStop) cachedSelect = 2;
    if(speedStopped) cachedSpeed = 0;
    if(disabled) cachedEnable = 0;
    if(!speedStopped || !disabled || !selectedStop || !probeStopped) retrySetup(2);
    else if(driveAlarm == 0) currentStatus =
        setupPhase == Setup::Complete && monitorValid && alarmValid && !kfm ? MOTOR_READY : MOTOR_NOT_READY;
    return speedStopped && disabled && selectedStop && probeStopped;
}

void sleepTse() { off(); }
void resetTse() {
    off();
    initTse();
}
}

const TseDiagnostics& motor_tse_diagnostics() {
    diagnostics.phase = phaseName();
    diagnostics.identified = identified;
    diagnostics.kfm = kfm;
    diagnostics.motionArmed = motionArmed;
    diagnostics.monitorValid = monitorValid;
    diagnostics.alarmValid = alarmValid;
    diagnostics.pendingRegister = pendingReg;
    diagnostics.pending = pending;
    diagnostics.commandRpm = cachedSpeed;
    diagnostics.primaryDirectionSign = kfm ? -1 : 1;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    diagnostics.runCandidate = true;
    diagnostics.zeroEnableReadbackMask = zeroEnableReadbackMask;
    diagnostics.stopReadbackMask = stopReadbackMask;
    diagnostics.stopReadbackVerified = stopReadbackMask == 63 && motor_currentRPM == 0 && monitorValid && alarmValid && driveAlarm == 0;
#endif
    diagnostics.torqueScalingVerified = false; // no hardware scale calibration recorded
    diagnostics.torqueRawValid = monitorValid;
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(zeroProbe.active()) diagnostics.torqueRawValid = false;
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if(pa25Probe.active())diagnostics.torqueRawValid=false;
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    if(pa25Pulse.active())diagnostics.torqueRawValid=false;
#endif
    return diagnostics;
}

bool motor_tse_zero_probe_available() {
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    return true;
#else
    return false;
#endif
}
bool motor_tse_zero_probe_write_window() {
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    return zeroProbeSonWindow && zeroProbe.active();
#else
    return false;
#endif
}
const kfm_zero_probe::View& motor_tse_zero_probe_view() {
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    return zeroProbe.view();
#else
    static const kfm_zero_probe::View unavailable = {};
    return unavailable;
#endif
}
bool motor_tse_zero_probe_start() {
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(!kfm || !identified || setupPhase != Setup::Complete || zeroProbe.active() ||
       !monitorValid || !alarmValid || driveAlarm != 0 || cachedSelect != 2 ||
       !motor_tse_begin_diagnostic_read()) return false;
    motionArmed = false;
    return zeroProbe.start(millis());
#else
    return false;
#endif
}
bool motor_tse_zero_probe_cancel() {
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    pending = false; commCounter = 0;
    const bool stopped = zeroProbe.cancel(zeroProbeIo);
    if(stopped) {
        cachedSpeed = 0; cachedEnable = 0; cachedSelect = 2;
        monitorValid = false; alarmValid = false; pollStep = 0;
    }
    return stopped;
#else
    return false;
#endif
}

bool motor_tse_begin_diagnostic_read() {
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    if(pa25Pulse.active()||(pa25Pulse.view().writeAttempted&&(!pa25Pulse.view().restoreAcknowledged||!pa25Pulse.view().restoreReadbackValid)))return false;
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if(pa25Probe.active()||(pa25Probe.view().writeAttempted&&(!pa25Probe.view().restoreAcknowledged||!pa25Probe.view().restoreReadbackValid)))return false;
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(zeroProbe.active()) return false;
#endif
    // Called synchronously by comm_debug_raw_read_uart, before it changes UART
    // settings or consumes a reply. Only a read can be surrendered; never lose
    // an outstanding control acknowledgement, or pause monitoring during a run.
    if(motionArmed || (state != IDLE && state != IDLE_GBW) || motor_setRPM != 0 || motor_currentRPM != 0 ||
       cachedSpeed != 0 || cachedEnable != 0 || (pending && pendingWrite)) return false;
    if(pending) {
        pending = false;
        commCounter = 0;
        ++diagnostics.cancelledPollReads;
    }
    ++diagnostics.diagnosticSessions;
    return true;
}

bool motor_tse_store_zero_default() {
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    return false;
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    return false; // persistent writes are excluded from the PA25 audit image
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if(zeroProbe.active()) return false;
#endif
    // Explicit commissioning action only: never write EEPROM automatically.
    if(!kfm || !identified || setupPhase != Setup::Complete || pending ||
       motionArmed || state != IDLE || !monitorValid || !alarmValid || driveAlarm != 0 ||
       motor_setRPM != 0 || motor_currentRPM != 0 ||
       cachedSpeed != 0 || cachedEnable != 0 || cachedSelect != 2) return false;
    return confirmBackendWrite(24, 0); // documented persistent PA24 address, only value zero
}

bool motor_tse_arm() {
#if !defined(SG98_TSE_KFM_ALLOW_MOTION) || !SG98_TSE_KFM_ALLOW_MOTION
    // Motion is opt-in; both shipped commissioning targets keep it locked.
    diagnostics.reason = "motion_compiled_out";
    return false;
#endif
    const auto& startup = diagnostics.startup;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(!candidateRemoteFresh() || stopReadbackMask != 63) return false;
    const bool startupSpeedSafe = startup.zeroPreset == 0;
    if(pending && !pendingWrite && !motor_tse_begin_diagnostic_read()) return false;
#else
    const bool startupSpeedSafe = startup.speedCommand == 0;
#endif
    if(!startup.captured || !startup.beforeAnyBackendWrite || !startupSpeedSafe ||
       startup.softwareEnable != 0 || startup.forcedInputs[0] != 0 ||
       startup.forcedInputs[1] != 2 || startup.actualRpm != 0) return false;
    if(!kfm || !identified || setupPhase != Setup::Complete || pending ||
       !monitorValid || !alarmValid || driveAlarm != 0 ||
       (state != IDLE && state != IDLE_GBW) || motor_setRPM != 0 || motor_currentRPM != 0 ||
       cachedSpeed != 0 || cachedEnable != 0 || cachedSelect != 2) return false;
    motionArmed = true;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    reversing = false; runClockActive = false; armedAt = millis();
#endif
    diagnostics.reason = "armed_build_speed_limit";
    currentStatus = MOTOR_READY;
    return true;
}

// Called only by an explicit user Start intent, never by the polling tick.
bool motor_tse_request_user_start() {
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
    if(error > 2 && error < 100) standaloneStopAckRequired = true;
    if(standaloneStopAckRequired || (state != IDLE && state != IDLE_GBW)) return false;
    return motor_tse_arm();
#else
    return false;
#endif
}

bool motor_tse_stop_ack_required() { return standaloneStopAckRequired; }

bool motor_tse_acknowledge_stop() {
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
    if(!kfm || !identified || setupPhase != Setup::Complete ||
       (state != IDLE && state != IDLE_GBW) || motionArmed ||
       motor_setRPM != 0 || motor_currentRPM != 0 || driveAlarm != 0 ||
       !monitorValid || !alarmValid || stopReadbackMask != 63 ||
       cachedSpeed != 0 || cachedEnable != 0 || cachedSelect != 2) return false;
    standaloneStopAckRequired = false;
    // A historical start watchdog is acknowledged only after a verified stop.
    // This does not write an alarm reset or clear a live drive alarm.
    if(error == 6) error = 0;
    return true;
#else
    return false;
#endif
}

void motor_tse_disarm() { off(); }

const MotorBackendDefinition MOTOR_BACKEND_TSE = {
    "tse", "TSE servo backend",
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    "KFM PA25 operator-present run candidate; variable speed/purge/GBW require final hardware validation.",
#else
    "TSE family backend plus bounded KFM commissioning tests; normal operation remains hardware-unvalidated.",
#endif
    "Fingerprint: family 40..80 or KFM 6/0x1F5C/encoder4; slave 1, 9600 8N2.",
    "KFM factory SON/SP1: boot locked; profile-specific explicit start/arming and bounded speed; family profile uses PA53.",
    "0x1000 signed rpm; KFM 0x1007 torque retained raw without verified scaling; mode 0x1009 and alarm 0x1013. No automatic alarm reset.",
    {probe, initTse, tick, off, nullptr, sleepTse, resetTse},
};

uint8_t motor_tse_commissioning_write_windows(){
 uint8_t windows=motor_tse_zero_probe_write_window()?1:0;
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
 if(runWriteWindow) windows |= 8;
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
 if(pulseWriteWindow)windows|=4;
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
 if(pa25WriteWindow)windows|=2;
#endif
 return windows;
}
bool motor_tse_pa25_probe_available(){
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
 return true;
#else
 return false;
#endif
}
const kfm_pa25_probe::View& motor_tse_pa25_probe_view(){
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
 return pa25Probe.view();
#else
 static const kfm_pa25_probe::View unavailable={};return unavailable;
#endif
}
bool motor_tse_pa25_probe_start(){
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
 if(!kfm||!identified||setupPhase!=Setup::Complete||pa25Probe.active()||!monitorValid||!alarmValid||driveAlarm||cachedSelect!=2||!motor_tse_begin_diagnostic_read())return false;
 motionArmed=false;return pa25Probe.start(millis());
#else
 return false;
#endif
}
bool motor_tse_pa25_probe_cancel(){
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
 pending=false;commCounter=0;const bool stopped=pa25Probe.cancel(pa25Io);
 if(stopped){cachedSpeed=0;cachedEnable=0;cachedSelect=2;monitorValid=false;alarmValid=false;pollStep=0;}
 return stopped;
#else
 return false;
#endif
}

bool motor_tse_pa25_probe_recover() {
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if(pa25Probe.active() || motionArmed || state != IDLE || motor_setRPM != 0 ||
       (pending && pendingWrite)) return false;
    // This is deliberately independent of setup==Complete: an orphan marker
    // after controller reset correctly prevents normal setup from completing.
    pending = false;
    commCounter = 0;
    monitorValid = false;
    alarmValid = false;
    uint16_t model[2] = {}, encoder = 0, communication[3] = {};
    const bool identityOk = pa25Read(1, 2, model) && model[0] == 6 && model[1] == 0x1F5C &&
        pa25Read(62, 1, &encoder) && encoder == 4 &&
        pa25Read(71, 3, communication) && communication[0] == 1 &&
        communication[1] == 96 && communication[2] == 0;
    if(!identityOk) {
        diagnostics.reason = "recovery_identity_unconfirmed";
        return false;
    }
    kfm = true; // fresh fingerprint verified; account recovery writes in startup audit
    const bool stopped = pa25Probe.recover(pa25Io);
    diagnostics.reason = pa25Probe.view().reason;
    if(stopped) {
        cachedSpeed = 0;
        cachedEnable = 0;
        cachedSelect = 2;
        setupPhase = Setup::Identity;
        identified = false;
    }
    return stopped;
#else
    return false;
#endif
}

bool motor_tse_pa25_pulse_available(){
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
 return true;
#else
 return false;
#endif
}
const kfm_pa25_pulse::View& motor_tse_pa25_pulse_view(){
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
 return pa25Pulse.view();
#else
 static const kfm_pa25_pulse::View unavailable={};return unavailable;
#endif
}
static bool startPa25PulseChecked(){
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
 const auto& startup=diagnostics.startup;
 if(!startup.captured||!startup.beforeAnyBackendWrite||startup.validMask!=15||startup.zeroPreset!=0||startup.softwareEnable!=0||startup.forcedInputs[0]!=0||startup.forcedInputs[1]!=2||startup.actualRpm!=0)return false;
 if(!kfm||!identified||setupPhase!=Setup::Complete||pa25Pulse.active()||!monitorValid||!alarmValid||driveAlarm||cachedSelect!=2||!motor_tse_begin_diagnostic_read())return false;
 return pa25Pulse.start(millis());
#else
 return false;
#endif
}
bool motor_tse_pa25_pulse_start(){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 return false; // touchscreen Start after one-shot USB arm is the only run path
#else
 return startPa25PulseChecked();
#endif
}
bool motor_tse_pa25_pulse_cancel(){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 touchArmed=false;touchRunActive=false;motor_setRPM=0;state=IDLE;
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
 pending=false;commCounter=0;const bool ok=pa25Pulse.cancel(pulseIo);
 if(ok){cachedSpeed=0;cachedEnable=0;cachedSelect=2;monitorValid=false;alarmValid=false;pollStep=0;}
 return ok;
#else
 return false;
#endif
}
bool motor_tse_pa25_pulse_recover(){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 if(touchArmed||touchRunActive)return false;
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
 if(pa25Pulse.active()||motionArmed||state!=IDLE||motor_setRPM!=0||(pending&&pendingWrite))return false;
 pending=false;commCounter=0;monitorValid=false;alarmValid=false;
 uint16_t model[2]={},encoder=0,communication[3]={};
 const bool matched=pulseRead(1,2,model)&&model[0]==6&&model[1]==0x1F5C&&pulseRead(62,1,&encoder)&&encoder==4&&pulseRead(71,3,communication)&&communication[0]==1&&communication[1]==96&&communication[2]==0;
 if(!matched){diagnostics.reason="pulse_recovery_identity_unconfirmed";return false;}
 kfm=true;const bool ok=pa25Pulse.recover(pulseIo);diagnostics.reason=pa25Pulse.view().reason;
 if(ok){cachedSpeed=0;cachedEnable=0;cachedSelect=2;setupPhase=Setup::Identity;identified=false;}
 return ok;
#else
 return false;
#endif
}

TseTouchTestView motor_tse_touch_test_view(){
 TseTouchTestView view;
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 view.available=true;view.armed=touchArmed;view.runActive=touchRunActive;
 view.remoteFresh=touchRemoteFresh();view.reason=touchReason;view.armedAtMs=touchArmedAt;
#endif
 return view;
}
bool motor_tse_touch_test_arm(){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 const auto& boot=diagnostics.startup;
 if(touchArmed||touchRunActive||pa25Pulse.active()||!touchRemoteFresh()||!kfm||!identified||setupPhase!=Setup::Complete||!monitorValid||!alarmValid||driveAlarm||state!=IDLE||motor_setRPM!=0||motor_currentRPM!=0||!boot.captured||!boot.beforeAnyBackendWrite||boot.validMask!=15||boot.zeroPreset!=0||boot.softwareEnable!=0||boot.forcedInputs[0]!=0||boot.forcedInputs[1]!=2||boot.actualRpm!=0||!motor_tse_begin_diagnostic_read())return false;
 touchArmed=true;touchArmedAt=millis();touchReason="armed_one_touch_start_100rpm_5s";currentStatus=MOTOR_READY;return true;
#else
 return false;
#endif
}
bool motor_tse_touch_test_request_start(){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 if(!touchArmed||touchRunActive||!touchRemoteFresh()||static_cast<uint32_t>(millis()-touchArmedAt)>=30000||state!=IDLE)return false;
 if(!startPa25PulseChecked())return false;
 touchArmed=false;touchRunActive=true;touchReason="touchscreen_start_accepted";return true;
#else
 return false;
#endif
}
bool motor_tse_touch_test_request_stop(){
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 touchArmed=false;touchRunActive=false;motor_setRPM=0;state=IDLE;
 const bool ok=motor_tse_pa25_pulse_cancel();currentStatus=MOTOR_NOT_READY;
 touchReason=ok?"touchscreen_stop_verified":"stop_unconfirmed_use_main_switch";return ok;
#else
 return false;
#endif
}
bool motor_tse_touch_test_disarm(){return motor_tse_touch_test_request_stop();}
