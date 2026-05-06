#include "motorcontrol_jmc.h"
#include "motor_hal.h"
#include "motor_probe.h"
#include "main.h"
#include "pdata.h"
#include "comm.h"
#include "display.h"
#include "error_history.h"
#include <limits.h>

namespace {
uint16_t motor_currentStatus = 0;
unsigned long lastCommSend = 0;
// JMC comm on this grinder is a mixed map in practice:
// - vendor-specific setup/runtime registers such as 0x2101/0x2103/0x2502/0x2503
// - CiA402-style objects such as 0x6040/0x6041/0x606C/0x6077
//
// The local "JMC Manual RU-1.pdf" is primarily an EC/CiA402 manual, so it is
// not a drop-in description of the whole RS-485 path. It was still critical for
// one detail that the old code got wrong: 0x606C "Actual speed" is I32, so it
// must be read as TWO 16-bit registers and decoded as signed 32-bit data.
// Do not simplify 0x606C back to a single-register read.
constexpr uint16_t JMC_RPM_REG_SPEC = 2114;
constexpr uint16_t JMC_RPM_REG_LEGACY = 0x606C;
constexpr uint16_t JMC_MODE_RESPONSE_REG = 0x6061;
constexpr uint16_t JMC_DRIVE_ERROR_REG = 0x605E;
constexpr unsigned long JMC_RPM_LOG_ZERO_DELAY_MS = 2000;
constexpr uint8_t JMC_RPM_MAX_MISSES = 2;
constexpr uint8_t JMC_MONITOR_PROBE_RETRIES = 2;
uint16_t jmcRpmReadReg = JMC_RPM_REG_LEGACY;
unsigned long jmcRpmSessionStartedAt = 0;
bool jmcRpmLoggedSample = false;
bool jmcRpmLoggedZero = false;
unsigned long jmcLastRpmReadAt = 0;
unsigned long jmcLastRpmRequestAt = 0;
uint8_t jmcRpmMissCount = 0;
bool jmcAwaitingRpmResponse = false;
enum JmcDiagReadType : uint8_t {
    JMC_DIAG_NONE = 0,
    JMC_DIAG_MODE_RESPONSE = 1,
    JMC_DIAG_DRIVE_ERROR = 2,
};
JmcDiagReadType jmcPendingDiagRead = JMC_DIAG_NONE;
bool jmcAwaitingDiagResponse = false;
unsigned long jmcLastDiagRequestAt = 0;
uint8_t jmcLastLoggedModeResponse = 0xFF;
uint16_t jmcLastLoggedDriveError = 0xFFFF;

bool jmcRpmLogStateActive() {
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

uint8_t jmcRpmSourceId() {
    return jmcRpmReadReg == JMC_RPM_REG_SPEC
               ? ERROR_HISTORY_MOTOR_RPM_SRC_JMC_2114
               : ERROR_HISTORY_MOTOR_RPM_SRC_JMC_606C;
}

uint16_t alternateJmcRpmReg(uint16_t currentReg) {
    return currentReg == JMC_RPM_REG_LEGACY ? JMC_RPM_REG_SPEC : JMC_RPM_REG_LEGACY;
}

uint16_t jmcRpmRegisterCount(uint16_t reg) {
    return reg == JMC_RPM_REG_LEGACY ? 2 : 1;
}

bool probeJmcAtRate(JmcCommRate rate, MotorProbeReport* report) {
    if(report == nullptr) {
        return false;
    }

    report->jmc_status_word = false;
    report->jmc_magic_word = false;
    report->jmc_control_word = false;
    report->jmc_baud = 0;

    comm_set_jmc_rate(rate, true);
    motor_probe_set_profile(COMM_PROFILE_JMC);

    report->jmc_status_word = motor_probe_read_register(0x6041, 1, JMC_MONITOR_PROBE_RETRIES);
    if(!report->jmc_status_word) {
        report->jmc_magic_word = motor_probe_read_register(0x1010, 2, JMC_MONITOR_PROBE_RETRIES);
    }
    if(!report->jmc_status_word && !report->jmc_magic_word) {
        report->jmc_control_word = motor_probe_write_register(0x6040, 1);
    }

    if(report->jmc_status_word || report->jmc_magic_word || report->jmc_control_word) {
        report->jmc_baud = motor_probe_baud_from_jmc_rate(rate);
        return true;
    }

    return false;
}

int16_t decodeJmcRpmValue(uint16_t reg, const uint16_t* words) {
    if(words == nullptr) {
        return 0;
    }

    if(reg == JMC_RPM_REG_LEGACY) {
        // 0x606C is documented as I32 in the RU-1 EC/CiA402 manual. The working
        // serial path on this unit also needs that width; treating it as 16-bit
        // produced bogus 0/100 RPM reads and destabilized the comm loop.
        const int32_t raw = static_cast<int32_t>(
            (static_cast<uint32_t>(words[0]) << 16) |
            static_cast<uint32_t>(words[1]));
        if(raw > INT16_MAX) return INT16_MAX;
        if(raw < INT16_MIN) return INT16_MIN;
        return static_cast<int16_t>(raw);
    }

    return static_cast<int16_t>(words[0]);
}

uint32_t packMotorRpmLogValue(uint8_t subtype, uint8_t source, int16_t value) {
    return (static_cast<uint32_t>(subtype) << 24) |
           (static_cast<uint32_t>(source) << 16) |
           static_cast<uint16_t>(value);
}

uint32_t packMotorDiagLogValue(uint8_t subtype, uint16_t value) {
    return (static_cast<uint32_t>(subtype) << 24) | static_cast<uint16_t>(value);
}

void resetJmcRpmLogSession() {
    jmcRpmSessionStartedAt = 0;
    jmcRpmLoggedSample = false;
    jmcRpmLoggedZero = false;
}

void resetJmcRpmReadHealth() {
    jmcLastRpmReadAt = 0;
    jmcLastRpmRequestAt = 0;
    jmcRpmMissCount = 0;
    jmcAwaitingRpmResponse = false;
}

void resetJmcDiagReadState() {
    jmcPendingDiagRead = JMC_DIAG_NONE;
    jmcAwaitingDiagResponse = false;
    jmcLastDiagRequestAt = 0;
    jmcLastLoggedModeResponse = 0xFF;
    jmcLastLoggedDriveError = 0xFFFF;
}

void queueJmcDiagRead(JmcDiagReadType type) {
    if(type == JMC_DIAG_NONE) {
        return;
    }
    if(jmcAwaitingDiagResponse) {
        return;
    }
    if(jmcPendingDiagRead == JMC_DIAG_DRIVE_ERROR) {
        return;
    }
    jmcPendingDiagRead = type;
}

uint16_t jmcDiagRegister(JmcDiagReadType type) {
    switch(type) {
        case JMC_DIAG_MODE_RESPONSE: return JMC_MODE_RESPONSE_REG;
        case JMC_DIAG_DRIVE_ERROR: return JMC_DRIVE_ERROR_REG;
        case JMC_DIAG_NONE:
        default:
            return 0;
    }
}

void noteJmcModeResponse(uint16_t value) {
    const uint8_t mode = static_cast<uint8_t>(value & 0xFFU);
    if(mode == jmcLastLoggedModeResponse) {
        return;
    }
    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      ERROR_HISTORY_LEVEL_INFO,
                      ERROR_HISTORY_CODE_MOTOR_JMC_DIAG,
                      packMotorDiagLogValue(ERROR_HISTORY_MOTOR_DIAG_MODE_RESPONSE, mode),
                      state);
    jmcLastLoggedModeResponse = mode;
}

void noteJmcDriveError(uint16_t value) {
    if(value == jmcLastLoggedDriveError) {
        return;
    }
    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      value == 0 ? ERROR_HISTORY_LEVEL_INFO : ERROR_HISTORY_LEVEL_ERROR,
                      ERROR_HISTORY_CODE_MOTOR_JMC_DIAG,
                      packMotorDiagLogValue(ERROR_HISTORY_MOTOR_DIAG_DRIVE_ERROR, value),
                      state);
    jmcLastLoggedDriveError = value;
}

void switchJmcRpmRegister(uint16_t nextReg) {
    if(nextReg != JMC_RPM_REG_LEGACY && nextReg != JMC_RPM_REG_SPEC) {
        return;
    }
    jmcRpmReadReg = nextReg;
    resetJmcRpmReadHealth();
    motor_currentRPM = 0;
    if(commCounter > 0) {
        commCounter = 0;
    }
}

void noteJmcRpmRead(int16_t rpm) {
    jmcLastRpmReadAt = millis();
    jmcAwaitingRpmResponse = false;
    jmcRpmMissCount = 0;

    if(!jmcRpmLogStateActive() || motor_setRPM == 0) {
        return;
    }

    if(jmcRpmSessionStartedAt == 0) {
        jmcRpmSessionStartedAt = millis();
    }

    if(!jmcRpmLoggedSample && rpm != 0) {
        error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                          ERROR_HISTORY_LEVEL_INFO,
                          ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                          packMotorRpmLogValue(ERROR_HISTORY_MOTOR_RPM_SAMPLE, jmcRpmSourceId(), rpm),
                          state);
        jmcRpmLoggedSample = true;
    }
}

void noteJmcRpmReadFailure() {
    jmcAwaitingRpmResponse = false;
    if(jmcRpmMissCount < 0xFF) {
        jmcRpmMissCount++;
    }
    if(jmcRpmMissCount >= JMC_RPM_MAX_MISSES) {
        switchJmcRpmRegister(alternateJmcRpmReg(jmcRpmReadReg));
    }
}

void checkJmcRpmZeroLog() {
    if(!jmcRpmLogStateActive() || motor_setRPM == 0) {
        resetJmcRpmLogSession();
        resetJmcRpmReadHealth();
        return;
    }

    if(jmcRpmSessionStartedAt == 0) {
        jmcRpmSessionStartedAt = millis();
    }

    if(jmcAwaitingRpmResponse &&
       millis() - jmcLastRpmRequestAt >= static_cast<unsigned long>(comm_get_receive_delay_ms() + 180)) {
        noteJmcRpmReadFailure();
    }

    if(jmcAwaitingDiagResponse &&
       millis() - jmcLastDiagRequestAt >= static_cast<unsigned long>(comm_get_receive_delay_ms() + 180)) {
        jmcAwaitingDiagResponse = false;
        jmcPendingDiagRead = JMC_DIAG_NONE;
    }

    if(!jmcRpmLoggedSample && !jmcRpmLoggedZero &&
       millis() - jmcRpmSessionStartedAt >= JMC_RPM_LOG_ZERO_DELAY_MS &&
       motor_currentRPM == 0) {
        error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                          ERROR_HISTORY_LEVEL_WARN,
                          ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                          packMotorRpmLogValue(ERROR_HISTORY_MOTOR_RPM_ZERO,
                                               jmcRpmSourceId(),
                                               motor_get_effective_set_rpm()),
                          state);
        jmcRpmLoggedZero = true;
    }
}
}

int16_t reg_6040 = -1; //Control word

int16_t reg_0101 = -1; //mode torque = 2;
int16_t reg_0121 = -1; //stop torque %
int16_t reg_0102 = -1; //Rigidity source selection
int16_t reg_0103 = -1; //rigidity amount (0-31)

int16_t reg_0500 = -1; // torq command source = 1
int16_t reg_0501 = -1; // speed limit source = 1
int16_t reg_0502 = -1; // speed limit setpoint = RPM (uint)
int16_t reg_0503 = -1; // torque SET POINT (-300 to 300)
int16_t reg_0510 = -1; // internal fw torque limit (int 0-300)
int16_t reg_0511 = -1; // internal rv torque limit (int -300 - 0)

void resetJmcRuntimeRegisterMirror() {
  reg_6040 = -1;
  reg_0101 = -1;
  reg_0121 = -1;
  reg_0102 = -1;
  reg_0103 = -1;
  reg_0500 = -1;
  reg_0501 = -1;
  reg_0502 = -1;
  reg_0503 = -1;
  reg_0510 = -1;
  reg_0511 = -1;
}

void resetJmcStartCommandMirror() {
  reg_6040 = -1;
  reg_0502 = -1;
  reg_0503 = 0;
}

bool writeJmcRuntimeConfig(int16_t configuredRamp,
                           int16_t configuredTorque,
                           int16_t configuredReverseTorque) {
  int score = 0;
  if(writeConfirm(0x2101, 2)) { reg_0101 = 2; score++; } // torque control
  if(writeConfirm(0x2102, 1)) { reg_0102 = 1; score++; } // semi-auto rigidity source
  if(writeConfirm(0x2103, configuredRamp)) { reg_0103 = configuredRamp; score++; }
  if(writeConfirm(0x2121, 30)) { reg_0121 = 30; score++; }
  if(writeConfirm(0x2203, 0)) { score++; } // velocity feedforward gain
  if(writeConfirm(0x2204, 2000)) { score++; }
  if(writeConfirm(0x2500, 1)) { reg_0500 = 1; score++; } // torque command source
  if(writeConfirm(0x2501, 1)) { reg_0501 = 1; score++; } // speed limit source
  if(writeConfirm(0x2502, 0)) { reg_0502 = 0; score++; }
  if(writeConfirm(0x2503, 0)) { reg_0503 = 0; score++; }
  if(writeConfirm(0x2510, configuredTorque)) { reg_0510 = configuredTorque; score++; }
  if(writeConfirm(0x2511, configuredReverseTorque)) { reg_0511 = configuredReverseTorque; score++; }
  return score >= 12;
}

enum DriveStatus{
    SWITCH_ON_DISABLED,
    READY_TO_SWITCH_ON,
    SWITCHED_ON,
    OPERATION_ENABLED,
    FAULT,
    FAULT_REACTION_ACTIVE,
    QUICK_STOP_ACTIVE,
    UNKNOWN_STATUS
};

DriveStatus decodeStatusWord(uint16_t statusWord) {
    // Extract relevant bits (0-6) from the status word
    uint8_t stateBits = statusWord & 0x006F; // bitmask 0b0000000001101111 (0, 1, 2, 3, 5, and 6)

    switch (stateBits) {
        case 0x0000: // 0b0000000
            return SWITCH_ON_DISABLED;
        case 0x0021: // 0b0100001
            return READY_TO_SWITCH_ON;
        case 0x0023: // 0b0100011
            return SWITCHED_ON;
        case 0x0027: // 0b0100111
            return OPERATION_ENABLED;
        case 0x0008: // 0b0001000
            return FAULT;
        case 0x000F: // 0b0001111
            return FAULT_REACTION_ACTIVE;
        case 0x0007: // 0b0000111
            return QUICK_STOP_ACTIVE;
        default:
            return UNKNOWN_STATUS;
    }
}

SYSTEM_STATUS changeToStatus(DriveStatus decodedStatusWord) { 
    switch(decodedStatusWord) { 
      case SWITCH_ON_DISABLED: return MOTOR_NOT_READY;
      case READY_TO_SWITCH_ON: return MOTOR_READY;
      case SWITCHED_ON: return MOTOR_READY;
      case OPERATION_ENABLED: return MOTOR_ENABLED;
      case FAULT: return MOTOR_FAULT;
      case FAULT_REACTION_ACTIVE: return MOTOR_FAULT;
      case QUICK_STOP_ACTIVE: return MOTOR_FAULT;
      default: return MOTOR_INVALID;
    }
}

void motor_jmc_init_backend() {
  motor_setRPM = 0;
  motor_currentRPM = 0;
  resetJmcRuntimeRegisterMirror();
  jmcRpmReadReg = JMC_RPM_REG_LEGACY;
  resetJmcRpmLogSession();
  resetJmcRpmReadHealth();
  resetJmcDiagReadState();
  
  commStatus = COMM_DISCONNECTED;
  currentStatus = MOTOR_NOT_CONNECTED;
}

bool motor_jmc_probe_backend(MotorProbeReport* report) {
  if(report == nullptr) {
    return false;
  }

  JmcCommRate probeRates[6] = {};
  const JmcCommRate originalRate = comm_get_jmc_rate();
  const uint8_t probeRateCount = motor_probe_build_jmc_order(originalRate, probeRates, 6);

  for(uint8_t i = 0; i < probeRateCount; ++i) {
    if(probeJmcAtRate(probeRates[i], report)) {
      comm_set_jmc_rate(originalRate, false);
      return true;
    }
  }

  comm_set_jmc_rate(originalRate, false);
  return false;
}

bool motor_jmc_do_comm_backend() { // can be called continously and will update all values. Returns true when new data packet is available.
    /* COMM TYPES
    0 = send RPM
    1 = read status word 0x6041
    2+ = read live RPM + torque
    */
  static unsigned long lastCommReceived;
  // static uint8_t lastCommType;
  static uint8_t nextCommType;
  static int16_t motor_lastSetRPM = -1;
  static int16_t lastEffectiveSetRPM = 0;
  const int16_t configuredRamp = motor_get_command_menu_value(MOTOR_COMMAND_RAMP_LEVEL);
  const int16_t configuredTorque = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, 1);
  const int16_t configuredReverseTorque = motor_get_formatted_command_value(MOTOR_COMMAND_TORQUE_LIMIT, -1);
  const int16_t effectiveSetRPM = motor_get_effective_set_rpm();

  //Disconnect flow
  // BLE scale activity can momentarily stretch the loop timing; use the explicit
  // disconnect budget instead of the idle polling interval to avoid false drops.
  if(lastCommReceived > 0 && lastCommReceived + DISCONNECTED_AFTER < millis()) { 
    #ifndef DEBUG
      error = 2;
    #endif

    #ifdef DEBUG_MOTORCONTROL
      Serial.print("Have not received a message for: "), Serial.print(millis()-lastCommReceived), Serial.println("ms -> DISCONNECTED");
    #endif
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
    disp_updateRequired = true;

    resetJmcRuntimeRegisterMirror();
  }

//Cannot establish connection 
  if(lastCommReceived == 0 && commStatus == COMM_DISCONNECTED && millis() > DISCONNECTED_AFTER) { 
    #ifndef DEBUG
      error = 1;
    #endif
    currentStatus = MOTOR_NOT_CONNECTED;
    disp_updateRequired = true;
  }

  //Unstable connection flow
  if(commStatus != COMM_DISCONNECTED && commCounter > 25 ) { 
    error = 4;
    motor_setRPM = 0;
    state = IDLE;
    disp_updateRequired = true;
  }

  // Motor stall flow
  const int16_t torquePercent = motor_get_torque_percent();
  if(motor_setRPM != 0 && torquePercent > 90 && abs(motor_currentRPM) < rpm_scalar) { 
    motor_setRPM = 0;
    state = IDLE; 
    error = 6;
  }

  checkJmcRpmZeroLog();

  if(effectiveSetRPM != 0 && lastEffectiveSetRPM == 0) {
    resetJmcStartCommandMirror();
    nextCommType = 0;
    resetJmcRpmLogSession();
    resetJmcRpmReadHealth();
  }
  lastEffectiveSetRPM = effectiveSetRPM;

  // First communication when disconnected
  if(commStatus == COMM_DISCONNECTED) { 

    const uint16_t reconnectDelay = controller_is_menu_state(state) ? comm_get_menu_delay_ms() : comm_get_send_delay_ms();
    if(lastCommSend + reconnectDelay > millis()) return false;

    if(writeConfirm(0x6040, 1)) { //This is blocking

      #ifdef DEBUG_MOTORCONTROL
        Serial.println("We're connected!.. ");
      #endif
      readReturn(0x1010, 2); //read magic byte setting MSB 
      // Check if magic byte is already set
      if(lastRead[0] != 0x6576 && lastRead[1] != 0x6173) { 
        if(!writeMultipleConfirm(0x1010, 2, 0x6576, 0x6173)) return false;  // magic byte "Save" to 0x1010 
        if(readReturn(0x2101, 1) != 2) error = 8;   //Confirm we've already written data or not.. to recognize a first boot event.
      }
      if(!writeJmcRuntimeConfig(configuredRamp, configuredTorque, configuredReverseTorque)) {
        error = 7;
        return false;
      }
      reg_6040 = 1;
      if(readReturn(JMC_RPM_REG_LEGACY, jmcRpmRegisterCount(JMC_RPM_REG_LEGACY)) != 0xFFFF) {
        jmcRpmReadReg = JMC_RPM_REG_LEGACY;
        motor_currentRPM = decodeJmcRpmValue(JMC_RPM_REG_LEGACY, lastRead);
        noteJmcRpmRead(motor_currentRPM);
      } else {
        jmcRpmReadReg = JMC_RPM_REG_SPEC;
        const int16_t specRpm = readReturn(JMC_RPM_REG_SPEC, jmcRpmRegisterCount(JMC_RPM_REG_SPEC));
        if(specRpm != 0xFFFF) {
          motor_currentRPM = decodeJmcRpmValue(JMC_RPM_REG_SPEC, lastRead);
          noteJmcRpmRead(motor_currentRPM);
        }
      }
      queueJmcDiagRead(JMC_DIAG_MODE_RESPONSE);
      lastCommReceived = millis();
      nextCommType = 0;
      commStatus = COMM_CONNECTED;
      currentStatus = MOTOR_NOT_READY;
      commCounter = 0;
    }
    lastCommSend = millis();
    return false;
  }

  // The only FAST & blocking service we're offering!
  if(motor_setRPM == 0 && motor_lastSetRPM != 0) { 
     if(reg_0503 != 0) {
      if(writeConfirm(0x2503, 0)) { // Set torque command to 0.
        lastCommSend = millis(); 
        reg_0503 = 0;
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }
    motor_lastSetRPM = motor_setRPM;
    nextCommType = 0;
    return false;
  }

  // >>>>>>>> RECEIVE <<<<<<<< 
  int16_t received = receive();
  if(received > 0) {

    // STATUS UPDATING
    commStatus = COMM_CONNECTED;
    if(currentStatus == MOTOR_NOT_CONNECTED) currentStatus = MOTOR_NOT_READY;
    lastCommReceived = millis(); 

    #ifdef DEBUG_MOTORCONTROL
      Serial.print("received!: "), Serial.println(received);
      Serial.print("Commcounter: "), Serial.println(commCounter);
      Serial.print("next comm type: "), Serial.println(nextCommType);

      if(lastRequestType == 0 || lastRequestType > 200) {
        Serial.println("received write succesfull!");
        Serial.print("Addr: "), Serial.print(received, HEX); 
        if(lastRequestType == 0) { Serial.print(" value: "), Serial.println(lastRead[0]); } 
        else { Serial.println(""); } 
      }
    #endif
    // ERROR HANDLING 
    if(error == 2 || error == 1) { 
      error = 0;
      disp_updateRequired = true;
    }

    // >>>>>> SEND SINGLE WRITE
    if(lastRequestType == 0) {  
      switch(received) {
        case 0x6040: { reg_6040 = lastRead[0]; break; } 

        case 0x2103: { reg_0103 = lastRead[0]; break; }

        case 0x2500: { reg_0500 = lastRead[0]; break; }
        case 0x2501: { reg_0501 = lastRead[0]; break; }
        case 0x2502: { reg_0502 = lastRead[0]; break; }
        case 0x2503: { reg_0503 = lastRead[0]; break; }
        case 0x2510: { reg_0510 = lastRead[0]; break; }
        case 0x2511: { reg_0511 = lastRead[0]; break; }
        default: break;
      }
    }

    // >>>>> DATA REQUESTS
    if(lastRequestType > 100 && lastRequestType < 200) { 
      #ifdef DEBUG_MOTORCONTROL 
        #ifndef DEBUG_COMM
        Serial.println("Newdata from read!");
        Serial.print("Addr: "), Serial.print(received, HEX), Serial.print(" value: "), Serial.println(lastRead[0]);
        if(received > 101) Serial.print("Addr: "), Serial.print(received+1, HEX), Serial.print(" value: "), Serial.println(lastRead[1]);
        if(received > 102) Serial.print("Addr: "), Serial.print(received+2, HEX), Serial.print(" value: "), Serial.println(lastRead[2]);  
        if(received > 103) Serial.print("Addr: "), Serial.print(received+3, HEX), Serial.print(" value: "), Serial.println(lastRead[3]);   
        #endif      
      #endif

      if(received == JMC_RPM_REG_SPEC || received == JMC_RPM_REG_LEGACY) {
        jmcRpmReadReg = received;
        motor_currentRPM = decodeJmcRpmValue(received, lastRead);
        noteJmcRpmRead(motor_currentRPM);
        disp_updateRequired = true;
      }
      if(received == JMC_MODE_RESPONSE_REG) {
        jmcAwaitingDiagResponse = false;
        jmcPendingDiagRead = JMC_DIAG_NONE;
        noteJmcModeResponse(lastRead[0]);
      }
      if(received == JMC_DRIVE_ERROR_REG) {
        jmcAwaitingDiagResponse = false;
        jmcPendingDiagRead = JMC_DIAG_NONE;
        noteJmcDriveError(lastRead[0]);
      }
      if(received == 0x603F && jmcAwaitingRpmResponse) {
        noteJmcRpmReadFailure();
      }
      if(received == 0x6077) { 
        motor_currentTorque = lastRead[0];
        newData = true;
      }

      if(received == 0x6041) { 
        const SYSTEM_STATUS previousStatus = currentStatus;
        currentStatus = changeToStatus(decodeStatusWord(lastRead[0]));
        if(currentStatus != previousStatus && currentStatus == MOTOR_FAULT) {
          queueJmcDiagRead(JMC_DIAG_DRIVE_ERROR);
        }
        disp_updateRequired = true;
      }
    }

    // >>>>> Send multiple writes
    if(lastRequestType > 200 && lastRequestType < 205) { //A SEND of max 4 writes -->> 
      // does not work properly on JMC factory registries... registries are not continouous?? reading not either
      // Does work for 32 bit registers like 0x6081 example:
      // if(received == 0x6081) { 
      //   reg_6081 = pending_reg_6081;
      //   reg_6082 = pending_reg_6082;
      // }
    } 

    return true;
  } else newData = false; // We've made one complete round!

  //Determine COMM delay
  uint16_t delay;
  if(effectiveSetRPM == 0) if(reg_6040!=3 && reg_0503!=0 && reg_0502 != 0 && nextCommType > 0) nextCommType = 0; //We need to SEND
  if (nextCommType == 0) delay = comm_get_send_delay_ms(); 
  else if(nextCommType > 0) { 
    if(controller_is_menu_state(state)) delay = comm_get_menu_delay_ms();
    else if (state == IDLE || state == IDLE_GBW) delay = comm_get_idle_delay_ms();
    else delay = comm_get_active_interval_ms();
  }
  if(lastCommSend + delay > millis()) return false;

// >>>>>> MOTOR OFF <<<<<<<<
if(effectiveSetRPM == 0) { 
    if(reg_0503 != 0) { 
      if(writeSingleRegister(0x2503, 0)) { // Set torq command to 0
        lastCommSend = millis(); 
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_0502 != 0) { 
      if(writeSingleRegister(0x2502, 0)) { // Set rotation speed 0
        lastCommSend = millis(); 
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_6040 != 3) { 
      if(writeSingleRegister(0x6040, 3)) { //SERVO DISABLE
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    // SET ONCE >> 
    if(reg_0103 != configuredRamp && (state == IDLE || state == IDLE_GBW)) { 
      if(writeConfirm(0x2103, configuredRamp)) { // Set rigidity LVL (restart required)
        reg_0103 = configuredRamp;
        error = 107; // reboot drive error
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_0510 != configuredTorque) { 
      if(writeSingleRegister(0x2510, configuredTorque)) { // Set fw torque limit 
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }
    if(reg_0511 != configuredReverseTorque) { 
      if(writeSingleRegister(0x2511, configuredReverseTorque)) { // Set fw torque limit 
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }
    // << SET ONCE
    
    // Check if we've done all commands.
    if(reg_6040 == 3 && reg_0502 == 0 && reg_0503 == 0 && reg_0101 == 2) { 
      if(nextCommType == 0) nextCommType = 1;
    }
  }

  //>>>>> Forward ROTATION <<<<<<<<<
  if(effectiveSetRPM > 0) { 
    if(reg_0502 != effectiveSetRPM) { 
      if(writeSingleRegister(0x2502, effectiveSetRPM)) { // Set fw speed
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_6040 != 0x07 && reg_6040 != 0x0F && reg_0502 == effectiveSetRPM && reg_0503 >= 0) {
      if(writeSingleRegister(0x6040, 0x07)) { // Switch on before enable
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_6040 != 0x0F && reg_6040 == 0x07 && reg_0502 == effectiveSetRPM && reg_0503 >= 0) { 
      if(writeSingleRegister(0x6040, 0x0F)) { //SERVO ENABLE
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_0503 != configuredTorque) { 
      if(writeSingleRegister(0x2503, configuredTorque)) { // Set fw torque target
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    //Check if we've done all commands
    if(reg_6040 == 0x0F && reg_0502 == effectiveSetRPM && reg_0503 == configuredTorque) { 
      if(nextCommType == 0) nextCommType = 1; 
    }
  }
  
  // >>>>>>>>> REVERSE ROTATION <<<<<<<<<
  if(effectiveSetRPM < 0) { 
    if(reg_0502 != -effectiveSetRPM) { 
      if(writeSingleRegister(0x2502, -effectiveSetRPM)) { // Set rpm limit
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_6040 != 0x07 && reg_6040 != 0x0F && reg_0502 == -effectiveSetRPM && reg_0503 <= 0) {
      if(writeSingleRegister(0x6040, 0x07)) { // Switch on before enable
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_6040 != 0x0F && reg_6040 == 0x07 && reg_0502 == -effectiveSetRPM && reg_0503 <= 0) { 
      if(writeSingleRegister(0x6040, 0x0F)) { //SERVO ENABLE
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    if(reg_0503 != configuredReverseTorque) { 
      if(writeSingleRegister(0x2503, configuredReverseTorque)) { // Set rv torque limit 
        lastCommSend = millis();
        nextCommType = 0;
        motor_lastSetRPM = motor_setRPM;
        return false;
      }
    }

    //Check if we've done all commands
    if(reg_6040 == 0x0F && reg_0502 == -effectiveSetRPM && reg_0503 == configuredReverseTorque) { 
      if(nextCommType == 0) nextCommType = 1; 
    }
  }

  // READING
  if(nextCommType == 1) { 
    nextCommType++;
    readRegister(0x6041, 1); //Read status word every 10
    lastCommSend = millis();
    return false;
  }

  if(nextCommType > 1 && nextCommType < 10) {
    if (nextCommType % 2 == 0) {
      nextCommType++;
      jmcAwaitingRpmResponse = true;
      jmcLastRpmRequestAt = millis();
      readRegister(jmcRpmReadReg, jmcRpmRegisterCount(jmcRpmReadReg));
      lastCommSend = millis();
    } else {
      nextCommType++;
      readRegister(0x6077, 1);
      lastCommSend = millis();
    }
  }

  if(nextCommType >= 10) { 
    if(jmcPendingDiagRead != JMC_DIAG_NONE && !jmcAwaitingDiagResponse) {
      const uint16_t reg = jmcDiagRegister(jmcPendingDiagRead);
      if(reg != 0) {
        if(readRegister(reg, 1)) {
          jmcAwaitingDiagResponse = true;
          jmcLastDiagRequestAt = millis();
          lastCommSend = millis();
          return false;
        }
      } else {
        jmcPendingDiagRead = JMC_DIAG_NONE;
      }
    }
    nextCommType = 0;
  }
  return false;
}

bool motor_jmc_off_backend() {
    for(int i = 0; i < 10; i++) {
      if(writeConfirm(0x6040, 3)) {
        lastCommSend = millis();
        reg_6040 = 3;
        return true;
      } 
      delay(200);
    }
    return false;
}

void motor_jmc_sleep_backend() { 
  #ifdef DEBUG_MOTORCONTROL
  Serial.print("Motor sleep..");
  #endif
    // Match the previous sleep flow first, then drop to the lowest-power
    // CiA-402 control word state documented by the JMC manual.
    motor_jmc_off_backend();
    for(int i = 0; i < 10; i++) {
      delay(200);
      if(writeConfirm(0x6040, 0)) { //Set control word to 0 = lowest power state possible.
        lastCommSend = millis();
        reg_6040 = 0;
        #ifdef DEBUG_MOTORCONTROL
          Serial.println("Servo drive set to low power state");
        #endif
        return;
      } 
    }
}

void motor_jmc_reset_backend() { 
  writeConfirm(0x2101, 1); // Reset control mode to speed mode
  writeMultipleConfirm(0x1010, 2, 0, 0); // Set magic byte to 0
  writeConfirm(0x6060, 3); //Set CiA402 to speedmode
  motor_jmc_sleep_backend(); //Should prevent the necessity of a restart, unsure tho.
}

//This function needs to be called continuously untill it returns true; which indicates a completed calibration
bool motor_jmc_calibrate_backend() { 
    static unsigned long lastCall;
    static unsigned long checkTime;
    static uint32_t sum = 0;
    static uint16_t sumCount = 0;

    if(lastCall + 3000 < millis()) { //reset after last call 2.5s ago. 
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

    if(commStatus != COMM_CONNECTED) { //Can't go here if comm is bad.
        error = 101; 
        state = MENU1;
        return false;
    }

    //All during calibration actions
    if(checkTime + 250 < millis() && newData == true) { // Collect current frames after 250 ms of stabilizations
        sum = sum + motor_currentTorque;
        sumCount++;
    }

    //All progression to next RPM actions
    if(currentCal < (SIZEOFCALIBRATEARRAY/sizeof(uint16_t)) && checkTime + CAL_TIME < millis() && sumCount != 0) { 
        calibrateArray[currentCal] = uint16_t(sum/sumCount);
        #ifdef DEBUG_CALIB
            Serial.print("Progressing!: "), Serial.println(currentCal);
            Serial.print("sumcount: "), Serial.println(sumCount);
            Serial.print("sum: "), Serial.println(sum);
            Serial.print("Av torq: "), Serial.println(sum/sumCount);
        #endif
        currentCal++;
        motor_setRPM = (currentCal*rpm_scalar + absolute_min_rpm) > absolute_max_rpm ? absolute_max_rpm : (currentCal*rpm_scalar + absolute_min_rpm);

        if(motor_setRPM < 0) { 
          #ifdef DEBUG_CALIB
            Serial.print("Motor set ERROR <0: "), Serial.println(motor_setRPM);
          #endif
          motor_setRPM = 0;
        }

        checkTime = millis(); 
        sum = 0;
        sumCount = 0;
        disp_updateRequired = true;
    }

    //Ending actions
    if(checkTime + CAL_TIME < millis() && currentCal >= (SIZEOFCALIBRATEARRAY/sizeof(uint16_t))) { //we have completed the last Calibration
        if(sumCount != 0) calibrateArray[currentCal] = uint16_t(sum/sumCount);
        motor_setRPM = 0;
        pdata_write(1);
        #ifdef DEBUG_CALIB
        Serial.println("Writing calib array!"); 
        for(int i = 0; i < SIZEOFCALIBRATEARRAY/2; i++) { 
          Serial.print("Array"), Serial.print(i), Serial.print(" value: "), Serial.println(calibrateArray[i]);
        }
        #endif
        state = IDLE;
        enterMenu = false; 
        menu1Selected = EXITMENU;
        return true;
    }

    return false;
}

const MotorBackendDefinition MOTOR_BACKEND_JMC = {
    "jmc",
    "JMC servo backend",
    "Torque-mode JMC backend with CiA402 and vendor register handling.",
    "Probe 0x6041 / 0x1010 / 0x6040 across the supported UART baud list until one path responds cleanly.",
    "Fill torque, ramp, speed, and save-register paths from the JMC programmer guide. Keep the current sign handling split between forward and reverse torque targets.",
    "Decode RPM from 0x606C as signed I32 across two registers and keep the 2114 fallback only when the legacy path fails.",
    {
        motor_jmc_probe_backend,
        motor_jmc_init_backend,
        motor_jmc_do_comm_backend,
        motor_jmc_off_backend,
        motor_jmc_calibrate_backend,
        motor_jmc_sleep_backend,
        motor_jmc_reset_backend,
    },
};
