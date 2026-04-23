#include "motor_hal.h"

#include <HardwareSerial.h>
#include <limits.h>

#include "comm.h"
#include "display.h"
#include "error_history.h"
#include "main.h"
#include "JMC/motorcontrol_jmc.h"
#include "RT/motorcontrol_rt.h"
#include "pdata.h"

int16_t motor_setRPM = 0;
int16_t motor_currentRPM = 0;
int16_t motor_currentTorque = 0;
int16_t calibrateArray[SIZEOFCALIBRATEARRAY] = {0};
uint16_t currentCal = 0;
int16_t commCounter = 0;
COMM_STATUS commStatus = COMM_DISCONNECTED;
SYSTEM_STATUS currentStatus = MOTOR_NOT_CONNECTED;
DriveType motorDriveType = DRIVE_TYPE_UNKNOWN;

namespace {
constexpr unsigned long MOTOR_PROBE_INTERVAL = 3000;
constexpr unsigned long PROFILE_SETTLE_MS = 20;
constexpr uint8_t MOTOR_MENU_NONE = 0xFF;
unsigned long lastProbeAt = 0;
DriveType lastProfileType = static_cast<DriveType>(-1);
uint32_t lastProbeLogValue = 0xFFFFFFFFUL;
bool autoProbeDone = false;
JmcCommRate preferredJmcRate = JMC_COMM_RATE_9600;
uint32_t lastJmcBaudLogValue = 0xFFFFFFFFUL;
JmcBaudTrialStatus jmcBaudTrialStatus = JMC_BAUD_SWITCH_IDLE;
uint32_t jmcBaudTrialResultBaud = 0;
uint32_t jmcBaudTrialReadBeforeBaud = 0;
uint32_t jmcBaudTrialReadAfterBaud = 0;

const MotorCommandDescriptor UNKNOWN_COMMANDS[] = {
    {MOTOR_COMMAND_SET_RPM, MOTOR_MENU_NONE, MOTOR_MENU_NONE, 1, false, "Set RPM", "rpm", "No live drive detected yet.", "Run AUTO detect or force a drive first."},
    {MOTOR_COMMAND_TORQUE_LIMIT, 1, static_cast<uint8_t>(SETMOTORTORQUE), 1, false, "Motor torque", "%", "Torque limit depends on the detected drive.", "Detect a drive before tuning torque."},
    {MOTOR_COMMAND_RAMP_LEVEL, 1, static_cast<uint8_t>(SETMOTORRAMP), 1, false, "Motor ramp", "lvl", "Ramp support depends on the detected drive.", "Detect a drive before tuning ramp."},
    {MOTOR_COMMAND_DIRECTION, 1, static_cast<uint8_t>(SETMOTORDIRECTION), 1, true, "Motor direction", "", "Sets the primary grind direction.", "Normal keeps forward positive. Reversed flips the command sign."},
};

const MotorCommandDescriptor JMC_COMMANDS[] = {
    {MOTOR_COMMAND_SET_RPM, MOTOR_MENU_NONE, MOTOR_MENU_NONE, 1, true, "Set RPM", "rpm", "Primary grind speed target for JMC.", "Keep the target inside the motor's stable grind range."},
    {MOTOR_COMMAND_TORQUE_LIMIT, 1, static_cast<uint8_t>(SETMOTORTORQUE), 1, true, "Motor torque", "%", "Forward and reverse torque ceiling for JMC.", "Higher values push harder but can increase stall load."},
    {MOTOR_COMMAND_RAMP_LEVEL, 1, static_cast<uint8_t>(SETMOTORRAMP), 1, true, "Motor ramp", "lvl", "JMC rigidity / ramp profile.", "Changing ramp requires a drive reboot to fully apply."},
    {MOTOR_COMMAND_DIRECTION, 1, static_cast<uint8_t>(SETMOTORDIRECTION), 1, true, "Motor direction", "", "Sets the primary grind direction.", "Normal keeps forward positive. Reversed flips the command sign."},
};

const MotorCommandDescriptor RT_COMMANDS[] = {
    {MOTOR_COMMAND_SET_RPM, MOTOR_MENU_NONE, MOTOR_MENU_NONE, 1, true, "Set RPM", "rpm", "Jog speed target for RT.", "Keep the target inside the motor's stable grind range."},
    {MOTOR_COMMAND_TORQUE_LIMIT, 1, static_cast<uint8_t>(SETMOTORTORQUE), 10, true, "Motor torque", "%", "Torque ceiling for RT servo control.", "Higher values hit harder. RT applies this with its internal x10 scaling."},
    {MOTOR_COMMAND_RAMP_LEVEL, 1, static_cast<uint8_t>(SETMOTORRAMP), 1, false, "Motor ramp", "lvl", "RT drive does not use the shared ramp setting.", "Ramp tuning is only available on supported drives like JMC."},
    {MOTOR_COMMAND_DIRECTION, 1, static_cast<uint8_t>(SETMOTORDIRECTION), 1, true, "Motor direction", "", "Sets the primary grind direction.", "Normal keeps forward positive. Reversed flips the command sign."},
};

const MotorDescriptor MOTOR_DESCRIPTORS[] = {
    {
        DRIVE_TYPE_UNKNOWN,
        "auto",
        "AUTO",
        "Auto-detect attached motor drive.",
        "Use AUTO to probe the RS-485 bus and lock onto the live drive.",
        COMM_PROFILE_RT,
        MOTOR_MAX_RPM_JMC,
        0,
        0,
        UNKNOWN_COMMANDS,
        static_cast<uint8_t>(sizeof(UNKNOWN_COMMANDS) / sizeof(UNKNOWN_COMMANDS[0])),
        nullptr,
    },
    {
        DRIVE_TYPE_JMC,
        "jmc",
        "JMC",
        "JMC servo using torque-mode control.",
        "Supports ramp tuning and JMC baud tools.",
        COMM_PROFILE_JMC,
        MOTOR_MAX_RPM_JMC,
        10,
        10,
        JMC_COMMANDS,
        static_cast<uint8_t>(sizeof(JMC_COMMANDS) / sizeof(JMC_COMMANDS[0])),
        &MOTOR_BACKEND_JMC,
    },
    {
        DRIVE_TYPE_RT,
        "rt",
        "RT",
        "RT servo using jog-speed control.",
        "Ramp tuning is not available on RT drives.",
        COMM_PROFILE_RT,
        MOTOR_MAX_RPM_RT,
        100,
        20,
        RT_COMMANDS,
        static_cast<uint8_t>(sizeof(RT_COMMANDS) / sizeof(RT_COMMANDS[0])),
        &MOTOR_BACKEND_RT,
    },
};

struct ProbeCycleResult {
    MotorProbeReport report;
    DriveType detectedType;
};

const MotorDescriptor* findMotorDescriptor(DriveType driveType) {
    for(const MotorDescriptor& descriptor : MOTOR_DESCRIPTORS) {
        if(descriptor.driveType == driveType) {
            return &descriptor;
        }
    }
    return &MOTOR_DESCRIPTORS[0];
}

const MotorCommandDescriptor* findCommandDescriptor(const MotorDescriptor* descriptor, MotorCommandId commandId) {
    if(descriptor == nullptr || descriptor->commands == nullptr) {
        return nullptr;
    }

    for(uint8_t i = 0; i < descriptor->commandCount; ++i) {
        const MotorCommandDescriptor& command = descriptor->commands[i];
        if(command.id == commandId) {
            return &command;
        }
    }

    return nullptr;
}

const MotorCommandDescriptor* findCommandDescriptorForMenu(const MotorDescriptor* descriptor,
                                                           uint8_t menuNum,
                                                           uint8_t itemIdx) {
    if(descriptor == nullptr || descriptor->commands == nullptr) {
        return nullptr;
    }

    for(uint8_t i = 0; i < descriptor->commandCount; ++i) {
        const MotorCommandDescriptor& command = descriptor->commands[i];
        if(command.menuNum == menuNum && command.itemIdx == itemIdx) {
            return &command;
        }
    }

    return nullptr;
}

const menuEntry* menuForNumber(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return Menu1;
        case 2: return Menu2;
        case 3: return Menu3;
        case 4: return Menu4;
        default: return nullptr;
    }
}

int16_t readCommandMenuValue(const MotorCommandDescriptor* command) {
    if(command == nullptr || !command->supported || command->menuNum == MOTOR_MENU_NONE) {
        return 0;
    }

    const menuEntry* menu = menuForNumber(command->menuNum);
    if(menu == nullptr) {
        return 0;
    }

    return menu[command->itemIdx].value;
}

JmcCommRate jmcRateFromBaud(uint32_t baud) {
    switch (baud) {
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

uint32_t baudFromJmcRate(JmcCommRate rate) {
    switch (rate) {
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

uint32_t packJmcBaudLogValue(uint8_t subtype, uint32_t baud) {
    return (static_cast<uint32_t>(subtype) << 24) | (baud & 0x00FFFFFFUL);
}

void logJmcBaudEvent(uint8_t subtype, uint32_t baud, uint8_t level) {
    const uint32_t value = packJmcBaudLogValue(subtype, baud);
    if(level == ERROR_HISTORY_LEVEL_INFO && value == lastJmcBaudLogValue) {
        return;
    }

    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      level,
                      ERROR_HISTORY_CODE_MOTOR_JMC_BAUD,
                      value,
                      static_cast<uint8_t>(state));
    lastJmcBaudLogValue = value;
}

uint32_t packJmcBaudTrialLogValue(uint8_t subtype, uint32_t baud) {
    return (static_cast<uint32_t>(subtype) << 24) | (baud & 0x00FFFFFFUL);
}

void logJmcBaudTrialEvent(uint8_t subtype, uint32_t baud, uint8_t level) {
    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      level,
                      ERROR_HISTORY_CODE_MOTOR_JMC_BAUD_TRIAL,
                      packJmcBaudTrialLogValue(subtype, baud),
                      static_cast<uint8_t>(state));
}

void rememberPreferredJmcRateDeferred(JmcCommRate rate, bool persist, bool logChange) {
    preferredJmcRate = rate;
    if(persist) {
        pdata_write_jmc_baud(baudFromJmcRate(rate));
    }
    if(logChange) {
        logJmcBaudEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_PREFERRED,
                        baudFromJmcRate(rate),
                        ERROR_HISTORY_LEVEL_INFO);
    }
}

uint32_t decodeJmcBaudSettingValue(uint16_t rawValue) {
    switch(rawValue) {
        case 0x0303:
        case 2:
            return 9600;
        case 0x0404:
        case 3:
            return 19200;
        case 0x0505:
        case 4:
            return 38400;
        case 0x0606:
        case 5:
            return 57600;
        case 0x0707:
        case 6:
            return 115200;
        default:
            return 0;
    }
}

bool readJmcBaudSetting(uint32_t* baudValue) {
    if(baudValue == nullptr) {
        return false;
    }

    if(readReturn(0x2024, 1) != 0xFFFF) {
        *baudValue = decodeJmcBaudSettingValue(lastRead[0]);
        return true;
    }

    if(readReturn(24, 1) != 0xFFFF) {
        *baudValue = decodeJmcBaudSettingValue(lastRead[0]);
        return true;
    }

    return false;
}

bool readJmcBaudSettingRaw(uint16_t* rawValue) {
    if(rawValue == nullptr) {
        return false;
    }

    if(readReturn(0x2024, 1) != 0xFFFF) {
        *rawValue = lastRead[0];
        return true;
    }

    if(readReturn(24, 1) != 0xFFFF) {
        *rawValue = lastRead[0];
        return true;
    }

    return false;
}

void rememberPreferredJmcRate(JmcCommRate rate, bool persist, bool logChange) {
    preferredJmcRate = rate;
    comm_set_jmc_rate(rate, false);
    if(persist) {
        pdata_write_jmc_baud(baudFromJmcRate(rate));
    }
    if(logChange) {
        logJmcBaudEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_PREFERRED,
                        baudFromJmcRate(rate),
                        ERROR_HISTORY_LEVEL_INFO);
    }
}

CommProfile profileForDriveType(DriveType driveType) {
    return findMotorDescriptor(driveType)->commProfile;
}

void resetMotorState() {
    motor_setRPM = 0;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    currentCal = 0;
    commCounter = 0;
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
}

uint32_t packProbeLogValue(const MotorProbeReport& report, DriveType detectedType) {
    uint32_t value = 0;
    if(report.jmc_status_word) value |= (1UL << 0);
    if(report.jmc_magic_word) value |= (1UL << 1);
    if(report.jmc_control_word) value |= (1UL << 2);
    if(report.rt_status_block) value |= (1UL << 3);
    value |= (static_cast<uint32_t>(detectedType) & 0xFFUL) << 8;
    if(report.jmc_baud == 9600) value |= (1UL << 16);
    if(report.jmc_baud == 19200) value |= (1UL << 17);
    if(report.jmc_baud == 38400) value |= (1UL << 18);
    if(report.jmc_baud == 57600) value |= (1UL << 19);
    if(report.jmc_baud == 115200) value |= (1UL << 20);
    return value;
}

ProbeCycleResult runProbeCycle(bool logResult, bool persistDetectedJmcRate) {
    ProbeCycleResult cycle = {};
    const uint8_t savedError = error;
    const uint16_t savedErrorCode = errorCode;
    const JmcCommRate initialPreferredJmcRate = preferredJmcRate;
    int bestPriority = -1;

    for(const MotorDescriptor& descriptor : MOTOR_DESCRIPTORS) {
        if(descriptor.backend == nullptr || descriptor.backend->api.probe == nullptr) {
            continue;
        }

        MotorProbeReport candidateReport = {};
        if(!descriptor.backend->api.probe(&candidateReport)) {
            continue;
        }

        if(static_cast<int>(descriptor.probePriority) < bestPriority) {
            continue;
        }

        bestPriority = descriptor.probePriority;
        cycle.report = candidateReport;
        cycle.detectedType = descriptor.driveType;
    }

    comm_set_jmc_rate(initialPreferredJmcRate, false);

    if(cycle.detectedType == DRIVE_TYPE_JMC) {
        const JmcCommRate detectedRate = jmcRateFromBaud(cycle.report.jmc_baud);
        rememberPreferredJmcRate(detectedRate,
                                 persistDetectedJmcRate,
                                 logResult && detectedRate != initialPreferredJmcRate);
        if(logResult) {
            logJmcBaudEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_DETECTED,
                            cycle.report.jmc_baud,
                            detectedRate == initialPreferredJmcRate
                                ? ERROR_HISTORY_LEVEL_INFO
                                : ERROR_HISTORY_LEVEL_WARN);
        }
    } else if(cycle.detectedType == DRIVE_TYPE_UNKNOWN) {
        cycle.detectedType = DRIVE_TYPE_UNKNOWN;
    }

    if(logResult) {
        const uint32_t probeValue = packProbeLogValue(cycle.report, cycle.detectedType);
        if(probeValue != lastProbeLogValue) {
            error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                              cycle.detectedType == DRIVE_TYPE_UNKNOWN
                                  ? ERROR_HISTORY_LEVEL_WARN
                                  : ERROR_HISTORY_LEVEL_INFO,
                              ERROR_HISTORY_CODE_MOTOR_PROBE,
                              probeValue,
                              static_cast<uint8_t>(state));
            lastProbeLogValue = probeValue;
        }
    }

    error = savedError;
    errorCode = savedErrorCode;
    return cycle;
}

DriveType detectDriveTypeReadOnly(bool persistDetectedJmcRate) {
    return runProbeCycle(true, persistDetectedJmcRate).detectedType;
}

void applyDriveProfile(bool persist) {
    if (lastProfileType == motorDriveType) return;

    Menu1[SETMAX].maxValue = motor_get_max_rpm();
    if (!motor_command_supported(MOTOR_COMMAND_RAMP_LEVEL)) {
        Menu1[SETMOTORRAMP].value = default_motor_ramp;
    }

    pdata_normalize(persist);
    disp_updateRequired = true;
    lastProfileType = motorDriveType;
}

void selectDriveType(DriveType detectedType, bool persist) {
    motorDriveType = detectedType;
    motor_currentRPM = 0;
    motor_currentTorque = 0;
    currentCal = 0;

    const MotorDescriptor* descriptor = findMotorDescriptor(motorDriveType);
    if(descriptor->backend == nullptr || descriptor->backend->api.init == nullptr) {
        resetMotorState();
    } else {
        comm_set_profile(descriptor->commProfile, true);
        delay(PROFILE_SETTLE_MS);
        descriptor->backend->api.init();
    }

    applyDriveProfile(persist);
}

void detectDriveTypeIfNeeded(bool force) {
    if (!force) {
        if(autoProbeDone) return;
        if(lastProbeAt + MOTOR_PROBE_INTERVAL > millis()) return;
    }

    const DriveType detectedType = detectDriveTypeReadOnly(true);
    lastProbeAt = millis();
    autoProbeDone = true;
    if (detectedType != motorDriveType) {
        selectDriveType(detectedType, true);
        if(detectedType != DRIVE_TYPE_UNKNOWN) {
            display_show_motor_probe_splash(motor_get_type_label());
        }
    }
}
}

void motor_init() {
    resetMotorState();
    rememberPreferredJmcRate(jmcRateFromBaud(pdata_read_jmc_baud()), false, false);
    autoProbeDone = false;
    selectDriveType(DRIVE_TYPE_UNKNOWN, false);
    detectDriveTypeIfNeeded(true);
}

bool do_comm() {
    const MotorDescriptor* descriptor = motor_get_current_descriptor();
    if(descriptor == nullptr || descriptor->backend == nullptr || descriptor->backend->api.tick == nullptr) {
        return false;
    }
    return descriptor->backend->api.tick();
}

bool motorOff() {
    const MotorDescriptor* descriptor = motor_get_current_descriptor();
    if(descriptor == nullptr || descriptor->backend == nullptr || descriptor->backend->api.off == nullptr) {
        motor_setRPM = 0;
        return false;
    }
    return descriptor->backend->api.off();
}

bool Calibrate() {
    const MotorDescriptor* descriptor = motor_get_current_descriptor();
    if(descriptor == nullptr || descriptor->backend == nullptr || descriptor->backend->api.calibrate == nullptr) {
        error = 101;
        state = MENU1;
        return false;
    }
    return descriptor->backend->api.calibrate();
}

void motorSleep() {
    const MotorDescriptor* descriptor = motor_get_current_descriptor();
    if(descriptor != nullptr && descriptor->backend != nullptr && descriptor->backend->api.sleep != nullptr) {
        descriptor->backend->api.sleep();
    }
}

void motorReset() {
    const MotorDescriptor* descriptor = motor_get_current_descriptor();
    if(descriptor != nullptr && descriptor->backend != nullptr && descriptor->backend->api.reset != nullptr) {
        descriptor->backend->api.reset();
    }
}

const MotorDescriptor* motor_get_descriptor(DriveType driveType) {
    return findMotorDescriptor(driveType);
}

const MotorDescriptor* motor_get_current_descriptor() {
    return findMotorDescriptor(motorDriveType);
}

const MotorBackendDefinition* motor_get_backend_definition(DriveType driveType) {
    const MotorDescriptor* descriptor = motor_get_descriptor(driveType);
    return descriptor != nullptr ? descriptor->backend : nullptr;
}

const MotorBackendDefinition* motor_get_current_backend_definition() {
    return motor_get_backend_definition(motorDriveType);
}

const MotorCommandDescriptor* motor_get_command_descriptor(DriveType driveType, MotorCommandId commandId) {
    return findCommandDescriptor(findMotorDescriptor(driveType), commandId);
}

const MotorCommandDescriptor* motor_get_current_command_descriptor(MotorCommandId commandId) {
    return motor_get_command_descriptor(motorDriveType, commandId);
}

const MotorCommandDescriptor* motor_get_command_descriptor_for_menu(DriveType driveType, uint8_t menuNum, uint8_t itemIdx) {
    return findCommandDescriptorForMenu(findMotorDescriptor(driveType), menuNum, itemIdx);
}

const MotorCommandDescriptor* motor_get_current_command_descriptor_for_menu(uint8_t menuNum, uint8_t itemIdx) {
    return motor_get_command_descriptor_for_menu(motorDriveType, menuNum, itemIdx);
}

DriveType motor_get_drive_type() {
    return motorDriveType;
}

const char* motor_get_type_key() {
    return motor_get_current_descriptor()->key;
}

const char* motor_get_type_label() {
    return motor_get_current_descriptor()->label;
}

const char* motor_get_type_description() {
    return motor_get_current_descriptor()->description;
}

const char* motor_get_type_guidance() {
    return motor_get_current_descriptor()->guidance;
}

const char* motor_get_backend_family_label() {
    const MotorBackendDefinition* backend = motor_get_current_backend_definition();
    return backend != nullptr ? backend->familyLabel : "";
}

const char* motor_get_backend_probe_guidance() {
    const MotorBackendDefinition* backend = motor_get_current_backend_definition();
    return backend != nullptr ? backend->probeGuidance : "";
}

const char* motor_get_backend_command_guidance() {
    const MotorBackendDefinition* backend = motor_get_current_backend_definition();
    return backend != nullptr ? backend->commandGuidance : "";
}

const char* motor_get_backend_telemetry_guidance() {
    const MotorBackendDefinition* backend = motor_get_current_backend_definition();
    return backend != nullptr ? backend->telemetryGuidance : "";
}

int16_t motor_get_max_rpm() {
    return motor_get_current_descriptor()->maxRpm;
}

bool motor_supports_ramp() {
    return motor_command_supported(MOTOR_COMMAND_RAMP_LEVEL);
}

bool motor_is_direction_reversed() {
    return motor_get_command_menu_value(MOTOR_COMMAND_DIRECTION) != 0;
}

int8_t motor_get_direction_multiplier() {
    return motor_is_direction_reversed() ? -1 : 1;
}

int16_t motor_apply_direction(int16_t rpm) {
    if(rpm == 0) {
        return 0;
    }

    // User-facing RPM targets stay positive for the primary grind direction.
    // The backend-specific command sign is derived here so JMC and RT both
    // honor the same "reverse direction" setting without changing higher-level
    // GbW or purge logic.
    return static_cast<int16_t>(rpm * motor_get_direction_multiplier());
}

int16_t motor_get_effective_set_rpm() {
    return motor_apply_direction(motor_setRPM);
}

int16_t motor_get_torque_percent() {
    const int16_t menuTorque = motor_get_command_menu_value(MOTOR_COMMAND_TORQUE_LIMIT);
    if(menuTorque <= 0) return 0;

    const MotorDescriptor* descriptor = motor_get_current_descriptor();
    if(descriptor == nullptr || descriptor->torquePercentNumerator <= 0) {
        return 0;
    }

    return static_cast<int16_t>((descriptor->torquePercentNumerator * motor_currentTorque) / menuTorque);
}

bool motor_command_supported(MotorCommandId commandId) {
    const MotorCommandDescriptor* command = motor_get_current_command_descriptor(commandId);
    return command != nullptr && command->supported;
}

int16_t motor_get_command_menu_value(MotorCommandId commandId) {
    return readCommandMenuValue(motor_get_current_command_descriptor(commandId));
}

int16_t motor_format_command_value(MotorCommandId commandId, int16_t uiValue, int8_t sign) {
    const MotorCommandDescriptor* command = motor_get_current_command_descriptor(commandId);
    if(command == nullptr || !command->supported) {
        return 0;
    }

    long formatted = static_cast<long>(uiValue) * static_cast<long>(command->valueScale);
    if(sign < 0) {
        formatted = -formatted;
    }

    if(formatted > INT16_MAX) return INT16_MAX;
    if(formatted < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(formatted);
}

int16_t motor_get_formatted_command_value(MotorCommandId commandId, int8_t sign) {
    return motor_format_command_value(commandId, motor_get_command_menu_value(commandId), sign);
}

void motor_force_drive_type(DriveType driveType, bool persist) {
    selectDriveType(driveType, persist);
    lastProbeAt = millis();
    autoProbeDone = true;
}

void motor_redetect_drive(bool persist) {
    autoProbeDone = false;
    selectDriveType(detectDriveTypeReadOnly(persist), persist);
    lastProbeAt = millis();
    autoProbeDone = true;
}

MotorProbeReport motor_run_probe_report() {
    const CommProfile originalProfile = comm_get_profile();
    const JmcCommRate originalJmcRate = comm_get_jmc_rate();
    const ProbeCycleResult cycle = runProbeCycle(false, false);
    comm_set_jmc_rate(originalJmcRate, false);
    comm_set_profile(originalProfile, true);
    return cycle.report;
}

uint32_t motor_get_preferred_jmc_baud() {
    return baudFromJmcRate(preferredJmcRate);
}

bool motor_set_preferred_jmc_baud(uint32_t baud_rate, bool persist) {
    const JmcCommRate nextRate = jmcRateFromBaud(baud_rate);
    const bool changed = nextRate != preferredJmcRate;
    rememberPreferredJmcRateDeferred(nextRate, persist, changed);
    return changed;
}

uint16_t packedJmcCommWordForBaud(uint32_t baud_rate) {
    switch(baud_rate) {
        case 9600: return 0x0303;
        case 19200: return 0x0404;
        case 38400: return 0x0505;
        case 57600: return 0x0606;
        case 115200: return 0x0707;
        default: return 0;
    }
}

bool motor_run_jmc_baud_switch(uint32_t targetBaud) {
    const uint32_t originalBaud = comm_get_jmc_baud();
    const uint16_t targetPackedSetting = packedJmcCommWordForBaud(targetBaud);
    jmcBaudTrialStatus = JMC_BAUD_SWITCH_RUNNING;
    jmcBaudTrialResultBaud = targetBaud;
    jmcBaudTrialReadBeforeBaud = 0;
    jmcBaudTrialReadAfterBaud = 0;
    disp_updateRequired = true;

    if(targetPackedSetting == 0) {
        jmcBaudTrialStatus = JMC_BAUD_SWITCH_FAIL_VERIFY;
        logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_VERIFY_FAIL,
                             targetBaud,
                             ERROR_HISTORY_LEVEL_ERROR);
        return false;
    }

    if(motorDriveType != DRIVE_TYPE_JMC || commStatus != COMM_CONNECTED) {
        jmcBaudTrialStatus = JMC_BAUD_SWITCH_FAIL_NO_JMC;
        jmcBaudTrialResultBaud = originalBaud;
        logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_NO_JMC,
                             originalBaud,
                             ERROR_HISTORY_LEVEL_WARN);
        return false;
    }

    if(comm_get_jmc_baud() == targetBaud) {
        rememberPreferredJmcRateDeferred(jmcRateFromBaud(targetBaud), true, preferredJmcRate != jmcRateFromBaud(targetBaud));
        jmcBaudTrialStatus = JMC_BAUD_SWITCH_ALREADY_SET;
        jmcBaudTrialResultBaud = targetBaud;
        jmcBaudTrialReadBeforeBaud = targetBaud;
        jmcBaudTrialReadAfterBaud = targetBaud;
        logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_ALREADY,
                             targetBaud,
                             ERROR_HISTORY_LEVEL_INFO);
        return true;
    }

    logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_STARTED,
                         originalBaud,
                         ERROR_HISTORY_LEVEL_INFO);

    uint16_t packedCommSetting = 0;
    if(readJmcBaudSettingRaw(&packedCommSetting)) {
        jmcBaudTrialReadBeforeBaud = decodeJmcBaudSettingValue(packedCommSetting);
        logJmcBaudEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_READ_BEFORE,
                        jmcBaudTrialReadBeforeBaud > 0 ? jmcBaudTrialReadBeforeBaud : packedCommSetting,
                        ERROR_HISTORY_LEVEL_INFO);
    } else {
        jmcBaudTrialReadBeforeBaud = originalBaud;
    }

    // The live JMC drive exposes P00-24 through 0x2024 as a mirrored packed
    // comm-profile word such as 0x0303, 0x0404, 0x0505, 0x0606, 0x0707.
    if(!writeConfirm(0x2024, targetPackedSetting)) {
        jmcBaudTrialStatus = JMC_BAUD_SWITCH_FAIL_WRITE;
        jmcBaudTrialResultBaud = originalBaud;
        logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_WRITE_FAIL,
                             targetBaud,
                             ERROR_HISTORY_LEVEL_ERROR);
        return false;
    }

    uint16_t readBackSetting = 0;
    if(!readJmcBaudSettingRaw(&readBackSetting)) {
        jmcBaudTrialStatus = JMC_BAUD_SWITCH_FAIL_VERIFY;
        jmcBaudTrialResultBaud = originalBaud;
        logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_VERIFY_FAIL,
                             targetBaud,
                             ERROR_HISTORY_LEVEL_ERROR);
        return false;
    }

    jmcBaudTrialReadAfterBaud = decodeJmcBaudSettingValue(readBackSetting);
    logJmcBaudEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_READ_AFTER,
                    jmcBaudTrialReadAfterBaud > 0 ? jmcBaudTrialReadAfterBaud : readBackSetting,
                    ERROR_HISTORY_LEVEL_INFO);

    if(readBackSetting != targetPackedSetting && jmcBaudTrialReadAfterBaud != targetBaud) {
        jmcBaudTrialStatus = JMC_BAUD_SWITCH_FAIL_VERIFY;
        jmcBaudTrialResultBaud = originalBaud;
        logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_VERIFY_FAIL,
                             targetBaud,
                             ERROR_HISTORY_LEVEL_ERROR);
        return false;
    }

    rememberPreferredJmcRateDeferred(jmcRateFromBaud(targetBaud), true, preferredJmcRate != jmcRateFromBaud(targetBaud));
    jmcBaudTrialStatus = JMC_BAUD_SWITCH_SET_OK;
    jmcBaudTrialResultBaud = targetBaud;
    logJmcBaudTrialEvent(ERROR_HISTORY_MOTOR_JMC_BAUD_TRIAL_OK,
                         targetBaud,
                         ERROR_HISTORY_LEVEL_INFO);
    disp_updateRequired = true;
    return true;
}

JmcBaudTrialStatus motor_get_jmc_baud_trial_status() {
    return jmcBaudTrialStatus;
}

uint32_t motor_get_jmc_baud_trial_result_baud() {
    return jmcBaudTrialResultBaud;
}

uint32_t motor_get_jmc_baud_trial_read_before() {
    return jmcBaudTrialReadBeforeBaud;
}

uint32_t motor_get_jmc_baud_trial_read_after() {
    return jmcBaudTrialReadAfterBaud;
}

const char* motor_get_jmc_baud_trial_status_text() {
    switch(jmcBaudTrialStatus) {
        case JMC_BAUD_SWITCH_RUNNING: return "RUNNING";
        case JMC_BAUD_SWITCH_SET_OK: return "SET BAUD REBOOT";
        case JMC_BAUD_SWITCH_ALREADY_SET: return "ALREADY SET";
        case JMC_BAUD_SWITCH_FAIL_NO_JMC: return "NO LIVE JMC";
        case JMC_BAUD_SWITCH_FAIL_WRITE: return "WRITE FAIL";
        case JMC_BAUD_SWITCH_FAIL_VERIFY: return "VERIFY FAIL";
        case JMC_BAUD_SWITCH_FAIL_LOST: return "LINK LOST";
        case JMC_BAUD_SWITCH_IDLE:
        default:
            return "READY";
    }
}
