#include <driver/rtc_io.h>
#include "esp_task_wdt.h"
#include "burr_gap.h"
#include "controller_board.h"
#include "controller_env.h"
#include "controller_ota.h"
#include "display.h"
#include "main.h"
#include "gbw.h"
#include "diag_console.h"
#include "error_history.h"
#include "motor_hal.h"
#include "pdata.h"
#include "display.h"
#include "io.h"
#include "comm.h"
#include "wifi_ui.h"
#include "radio_mode.h"
#include "remote_link.h"
#include "smart_purge_learning.h"
#include "sg98_version.h"

States state = IDLE;
int8_t menu1Selected = EXITMENU;
int8_t menu2Selected = RETURN_FROM_PURGE;
int8_t menu3Selected = RETURN_FROM_GBW;
int8_t menu4Selected = RETURN_FROM_JMC_BAUD;
int8_t menu5Selected = RETURN_FROM_WIRELESS;
int8_t motorSelectSelected = MOTOR_SELECT_AUTO;
bool enterMenu = false;
bool firstBootFlag = false;
States motorSelectReturnState = IDLE;

menuEntry Menu1[NUM_MENU1_ITEMS] = {
    {"EXIT MENU", 0, 0, 0, 0, "EXITMENU", ""},                 // 0
    {"Purge Settings", 0, 0, 0, 0, "PURGESETT", ""},           // 1
    {"GbW Settings", 0, 0, 0, 0, "GBWSETT", ""},               // 2
    {"Max RPM", default_maxRPM, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "MAXRPM", "rpm"}, // 3
    {"Min RPM", default_minRPM, absolute_min_rpm, 500, rpm_scalar, "MINRPM", "rpm"},              // 4
    {"Motor torque", default_motor_torque, 1, 300, 1, "MOTORTORQ", "%"},               // 5
    {"Motor ramp", default_motor_ramp, 0, 12, 1, "MOTORRMP", "lvl"}, // 6
    {"Motor direction", default_motor_direction_reversed, 0, 1, 1, "MOTORDIR", ""}, // 7
    {"Drive", 0, 0, 0, 0, "DRIVESEL", ""},                // 8
    {"JMC Baud", 0, 0, 0, 0, "JMCBAUDMENU", ""},               // 9
    {"Calibrate", 0, 0 ,0, 0, "CALIBRATE", ""},           // 10
    {"Gauge Set", 0, -1200, 1200, 1, "GAUGESET", ""},     // 11, value is 10um/count
    {"Wireless", default_radio_mode, 0, 2, 1, "RADIOMODE", ""}, // 12
    {"Reset Network", 0, 0, 1, 1, "RSTNET", ""},          // 13
    {"Sleep time", default_sleepTime, 0, 300, 5, "SLEEPT", "min"},// 14
    {"Brightness", default_brightness, 1, 255, 1, "BRIGHTNESS", ""}, // 15
    {"LED max Brightness", default_led_brightness, 0, 100, 1, "LEDPRCT", "%"}, // 16
    {"Invert scrolling", 0, 0, 1, 1, "SCROLL", ""},            // 17
    {"Reset default", 0, 0, 1, 1, "RSTALL", ""},          // 18
    {"Network info", 0, 0, 0, 0, "NETINFO", ""},          // 19
    {"About", 0, 0, 0, 0, "ABOUT", ""},                  // 20
};

menuEntry Menu2[NUM_MENU2_ITEMS] = {
    {"BACK", 0, 0, 0, 0, "RETURN2", ""},                                           // 0  RETURN_FROM_PURGE
    {"Auto purge", default_autoPurgeEnabled, 0, 1, 1, "AUTOPURGE", ""},    // 1  AUTO_PURGE_ENABLED
    {"Purge /w button", default_buttonPurge, 0, 1, 1, "PURGEBTN", ""},             // 2  SETBUTTONPURGE
    {"Purge forward RPM", default_purgeForwardRPM, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "PURGEFWRPM", "rpm" },  // 3  SETPURGERPM
    {"Reverse rotation", default_reverseRotation, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "RVRSRPM", "rpm"},            // 4  SETPURGEREVERSE
    {"Purge delay", default_purgeDelay, 100, 10000, 50, "PURGEDEL", "ms"},           // 5  SETPURGEDELAY
    {"Purge duration", default_purgeDuration, 500, 5000, 100, "PURGEDUR", "ms"},     // 6  SETPURGETIME
    {"Purge 0rpm time", default_purge_off_time, 0, 5000, 10, "PURGEOFFT", "ms"},     // 7  SETPURGEOFFTIME
    {"Purge stabilize time", default_purgeStabilTime, 100, 5000, 50, "PURGESTABILT" , "ms"}, // 8  SETPURGESTABILTIME
    {"Purge frames HIGH", default_purgeFramesHigh, 2, 20, 1, "PURGEFRH", ""},      // 9  SETPURGEFRAMESHIGH
    {"Purge frames LOW", default_purgeFramesLow, 2, 20, 1, "PURGEFRL", ""},        // 10 SETPURGEFRAMESLOW
    {"Purge Scalar HIGH", default_purgePrctHigh, 101, 200, 1, "PURGESCLRH", "%"},   // 11 SETPURGEPRCTHIGH
    {"Purge Scalar LOW", default_purgePrctLow, 101, 200, 1, "PURGESCLRL", "%"},     // 12 SETPURGEPRCTLOW
    {"Auto off time", default_auto_off_time, 0, 300, 10, "OFFTIME", "s"},           // 13 AUTO_OFF_TIME
};

menuEntry Menu3[NUM_MENU3_ITEMS] = {
    {"BACK", 0, 0, 0, 0, "RETURN3", ""},                            // 0 RETURN_FROM_GBW
    {"GbW RPM", default_GBWRPM, 200, 1500, rpm_scalar, "GBWRPMSET", "rpm"}, // 1 GBW_RPM_SET
    {"GbW Slow RPM", default_slow_rpm, rpm_scalar, 500, rpm_scalar, "GBWSLOWRPM", "rpm"}, // 2 GBW_SLOW_RPM
    {"GbW Slow mg", default_slow_mg, 0, 6000, 100, "GBWSLOWT", "mg"}, // 3 GBW_SLOW_PHASE GRAMS
    {"Start delay", default_start_delay, 0, 1000, 50, "GBWSTARTDEL", "ms"}, // 4 GBW_BUTTON_DELAY
    {"SpeedModifier", default_speedModifier, 1, 32767, 100, "GBWSPEEDMOD", ""},     // 5 GBW_SPEEDMOD
    {"Time offset", default_time_offset, 10, 1000, 10, "TIMEOFFSET", "ms"},           // 6 GBW_OFFSET
    {"Save scale", 0, 0, 1, 1, "SCALESTORED", ""},                                  // 7 SAVE_SCALE
};

menuEntry Menu4[NUM_MENU4_ITEMS] = {
    {"BACK", 0, 0, 0, 0, "RETURN4", ""},
    {"JMC 9600", 0, 0 ,0, 0, "JMC9600", ""},
    {"JMC 19200", 0, 0 ,0, 0, "JMC19200", ""},
    {"JMC 38400", 0, 0 ,0, 0, "JMC38400", ""},
    {"JMC 57600", 0, 0 ,0, 0, "JMC57600", ""},
    {"JMC 115200", 0, 0 ,0, 0, "JMC115200", ""},
};

menuEntry Menu5[NUM_MENU5_ITEMS] = {
    {"BACK", 0, 0, 0, 0, "RETURN5", ""},
    {"Mode", default_radio_mode, 0, 2, 1, "RADIOMODE", ""},
    {"Network info", 0, 0, 0, 0, "NETINFO", ""},
    {"Reset Network", 0, 0, 0, 0, "RSTNET", ""},
};

volatile bool newData = true; //This is new TORQUE data.

volatile uint8_t error = 0;
int16_t setRPM = default_setRPM;
uint32_t setWeight = default_setWeight;

unsigned long lastActivity = 0;

RTC_DATA_ATTR uint8_t rtc_bootFlag1 = 0;
RTC_DATA_ATTR bool modeGbW = false;
RTC_DATA_ATTR uint8_t rtc_state;
RTC_DATA_ATTR uint16_t rtc_setRPM;

bool ready_purge = false;

unsigned long lastLoopTime = 0;
int loopcount = 0;

namespace {
constexpr bool kDisableIdleSleepForTesting = false;
constexpr unsigned long MENU_TRAFFIC_POLL_INTERVAL_MS = 300;
constexpr unsigned long RADIO_OFF_DEEP_SLEEP_DELAY_MS = 15000;
constexpr unsigned long MOTOR_START_WATCHDOG_MS = 5000;
constexpr int16_t MOTOR_START_MIN_OBSERVED_RPM = rpm_scalar / 2;

uint8_t purgeFrameCounter = 0;
int16_t purgeLastSetRPM = 0;
unsigned long purgeStartTime = 0;
unsigned long purgeLastCheckAt = 0;
unsigned long purgeRunStartedAt = 0;
float purgeTestTorque = 0;
int16_t purgeTestScalar = 0;
int16_t purgeFrames = 0;
uint8_t purgePhase = 0;
uint8_t purgeLastPhase = 0xFF;
bool purgeSequenceNeedsInit = true;
bool idleSleepActive = false;
unsigned long lastMenuTrafficPollAt = 0;
unsigned long motorStartWatchdogStartedAt = 0;
int16_t motorStartWatchdogTargetRpm = 0;
States motorStartWatchdogState = IDLE;

bool controllerCanIdleSleep() {
    return state == IDLE || state == IDLE_GBW;
}

bool controllerHasLocalDeepSleepWake() {
    const ControllerBoardPins& pins = controller_board_pins();
    return controller_board_has_start_button() &&
           controller_board_pin_valid(pins.startButton) &&
           controller_board_gpio_valid(pins.wakeButton);
}

bool controllerRadioOffDeepSleepDue() {
    if(!radio_mode_is_off() || !controllerCanIdleSleep() || !controllerHasLocalDeepSleepWake()) {
        return false;
    }

    const unsigned long now = millis();
    return lastActivity + RADIO_OFF_DEEP_SLEEP_DELAY_MS < now &&
           now > RADIO_OFF_DEEP_SLEEP_DELAY_MS;
}

bool shouldPollTrafficNow(bool menuMode) {
    if(!menuMode) {
        lastMenuTrafficPollAt = millis();
        return true;
    }

    const unsigned long now = millis();
    if(lastMenuTrafficPollAt == 0 || now - lastMenuTrafficPollAt >= MENU_TRAFFIC_POLL_INTERVAL_MS) {
        lastMenuTrafficPollAt = now;
        return true;
    }

    return false;
}

uint16_t purgeCalibrationIndexForRpm(int16_t rpm) {
    int32_t rpmAbs = rpm;
    if(rpmAbs < 0) rpmAbs = -rpmAbs;

    uint16_t index = rpm_scalar == 0 ? 0 : static_cast<uint16_t>(rpmAbs / rpm_scalar);
    const uint16_t maxIndex = (SIZEOFCALIBRATEARRAY / sizeof(uint16_t)) - 1;
    if(index > maxIndex) index = maxIndex;
    return index;
}

uint16_t purgeBaselineTorqueForRpm(int16_t rpm) {
    const int16_t raw = calibrateArray[purgeCalibrationIndexForRpm(rpm)];
    return raw > 0 ? static_cast<uint16_t>(raw) : 0;
}

int16_t purgeScalarForRpm(int16_t rpm) {
    int32_t rpmAbs = rpm;
    if(rpmAbs < 0) rpmAbs = -rpmAbs;
    return rpmAbs > 500 ? Menu2[SETPURGEPRCTHIGH].value : Menu2[SETPURGEPRCTLOW].value;
}

uint16_t purgeManualThresholdForRpm(int16_t rpm, uint16_t baselineTorque) {
    const uint16_t scalar = static_cast<uint16_t>(purgeScalarForRpm(rpm));
    return static_cast<uint16_t>((static_cast<uint32_t>(baselineTorque) * scalar) / 100U);
}

void smartPurgeLearnFromMotor(int16_t targetRpm) {
    if(Menu2[AUTO_PURGE_ENABLED].value != true || newData != true || targetRpm == 0) return;

    const uint16_t baselineTorque = purgeBaselineTorqueForRpm(targetRpm);
    const uint16_t manualThreshold = purgeManualThresholdForRpm(targetRpm, baselineTorque);
    smart_purge_learning_note_sample(targetRpm,
                                     motor_currentRPM,
                                     motor_currentTorque,
                                     baselineTorque,
                                     manualThreshold);
}

bool motorStartWatchdogApplies(States currentState) {
    return currentState == GRINDING ||
           currentState == PURGING ||
           currentState == GRINDING_GBW;
}

uint8_t motorStartWatchdogRpmSource() {
    switch(motor_get_drive_type()) {
        case DRIVE_TYPE_JMC:
            return ERROR_HISTORY_MOTOR_RPM_SRC_JMC_606C;
        case DRIVE_TYPE_RT:
            return ERROR_HISTORY_MOTOR_RPM_SRC_RT_1301;
        case DRIVE_TYPE_KRS_130ST_M07725:
        case DRIVE_TYPE_UNKNOWN:
        default:
            return 0;
    }
}

uint32_t packMotorStartWatchdogValue(int16_t targetRpm) {
    return (static_cast<uint32_t>(ERROR_HISTORY_MOTOR_RPM_ZERO) << 24) |
           (static_cast<uint32_t>(motorStartWatchdogRpmSource()) << 16) |
           static_cast<uint16_t>(targetRpm);
}

void resetMotorStartWatchdog() {
    motorStartWatchdogStartedAt = 0;
    motorStartWatchdogTargetRpm = 0;
    motorStartWatchdogState = IDLE;
}

void tripMotorStartWatchdog() {
    const States failedState = state;
    const int16_t failedTarget = motorStartWatchdogTargetRpm;
    error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                      ERROR_HISTORY_LEVEL_ERROR,
                      ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                      packMotorStartWatchdogValue(failedTarget),
                      failedState);

    motor_setRPM = 0;
    if(failedState == GRINDING_GBW) {
        state = IDLE_GBW;
    } else {
        purge_cancel_pending();
        state = IDLE;
    }
    error = 6;
    disp_updateRequired = true;
    resetMotorStartWatchdog();
}

void pollMotorStartWatchdog() {
    const int16_t targetRpm = abs(motor_get_effective_set_rpm());
    if(!motorStartWatchdogApplies(state) ||
       targetRpm == 0 ||
       commStatus != COMM_CONNECTED ||
       currentStatus == MOTOR_FAULT) {
        resetMotorStartWatchdog();
        return;
    }

    if(abs(motor_currentRPM) >= MOTOR_START_MIN_OBSERVED_RPM) {
        resetMotorStartWatchdog();
        return;
    }

    const unsigned long now = millis();
    if(motorStartWatchdogStartedAt == 0 ||
       motorStartWatchdogTargetRpm != targetRpm ||
       motorStartWatchdogState != state) {
        motorStartWatchdogStartedAt = now;
        motorStartWatchdogTargetRpm = targetRpm;
        motorStartWatchdogState = state;
        return;
    }

    if(now - motorStartWatchdogStartedAt >= MOTOR_START_WATCHDOG_MS) {
        tripMotorStartWatchdog();
    }
}

void printWakeSummary() {
    Serial.print("Wakeup cause: ");
    Serial.println(esp_sleep_get_wakeup_cause());
    Serial.print("Reset reason: ");
    Serial.println(esp_reset_reason());

    if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("[sleep] source=start button");
    }
}
}

bool controller_is_menu_state(States currentState) {
    switch(currentState) {
        case MENU1:
        case MENU2:
        case MENU3:
        case MENU4:
        case MENU5:
        case MOTOR_SELECT:
        case NETWORK_INFO:
        case ABOUT_INFO:
            return true;
        default:
            return false;
    }
}

void purge_prepare_for_grinding() {
    smart_purge_learning_reset_runtime();
    purgeFrameCounter = 0;
    purgeLastSetRPM = setRPM;
    purgeStartTime = millis();
    purgeLastCheckAt = millis();
    ready_purge = false;
}

void purge_cancel_pending() {
    smart_purge_learning_reset_runtime();
    purgeFrameCounter = 0;
    purgeLastSetRPM = setRPM;
    purgeStartTime = millis();
    purgeLastCheckAt = millis();
    purgeRunStartedAt = millis();
    purgePhase = 0;
    purgeLastPhase = 0xFF;
    purgeSequenceNeedsInit = true;
    ready_purge = false;
}

void purge_mark_started() {
    smart_purge_learning_reset_runtime();
    purgeFrameCounter = 0;
    purgeLastSetRPM = setRPM;
    purgeStartTime = millis();
    purgeLastCheckAt = millis();
    purgeRunStartedAt = millis();
    purgePhase = 0;
    purgeLastPhase = 0xFF;
    purgeSequenceNeedsInit = true;
    ready_purge = false;
}


void scaleTask(void *pvParameters) {
    scales_init(); // Run once at task start

    // if (scaleMutex == NULL) {
    //     Serial.println("Failed to create scaleMutex!");
    //     error = 5;
    // }
    while (1) {
        gbwVitals();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void setup() { 
    Serial.begin(115200);
    controller_board_init();
    scaleMutex = xSemaphoreCreateMutex();
    if (scaleMutex == NULL) {
        #ifdef DEBUG 
            Serial.println("Failed to create mutex!");
        #endif
        error = 5;
    }
    #ifdef DEBUG
    delay(2000);
    Serial.println("Starting!");
    #else
    delay(250);
    #endif
    Serial.printf("[fw] SG98 controller %s prefs=%s\n", sg98_version::buildId().c_str(), BUILD_ID);
    Serial.printf("[fw] target=%s\n", controller_board_target_name());
    
    setCpuFrequencyMhz(80);

    if(rtc_bootFlag1 == true) { 
        sleep_release();
    }

    pdata_init(); //This sets state as well!
    Serial.println("pdata init");

    if(firstBootFlag == 1) { 
         pdata_write(0);
         Serial.println("Wrote all default values...");
    } else { 
        pdata_read(); 
        Serial.println("pdata read");
    }
    pdata_normalize();
    burr_gap_init();
    
    xTaskCreatePinnedToCore(
    scaleTask,         // Task function
    "ScaleTask",       // Name
    8192,              // Stack size (adjust as needed)
    NULL,              // Parameter
    1,                 // Priority
    NULL,              // Task handle
    0                  // Core 0 (use 1 for core 1)
    );

    #ifdef DEBUG
    printWakeSummary();
    #endif

    display_init();
    Serial.println("display init");
    io_init();
    comm_init();
    Serial.println("Comm init");
    error_history_init();
    motor_init();
    Serial.println("motor_init");
    diag_console_init();
    radio_mode_init();
    smart_purge_learning_init();
    vitals_init();
    controller_ota_init();
    lastActivity = millis();
}

void loop() { 
    doVitals();
    burr_gap_poll();
    do_io();
    const bool menuMode = controller_is_menu_state(state);
    if(menuMode) {
        update_display();
    }
    remote_link_safety_poll();
    do_comm();
    diag_console_poll();
    if(shouldPollTrafficNow(menuMode)) {
        radio_mode_poll();
    }
    controller_env_poll();
    sleepTimeCheck();
    if(idleSleepActive && !controllerCanIdleSleep()) {
        controller_exit_idle_sleep();
    }
    if(!menuMode) {
        update_display();
    }
    
    if(state == IDLE)
    { 
        motor_setRPM = 0;
    }

    if(state == IDLE_GBW) { 
        motor_setRPM = 0;
    }

    if(state == GRINDING)
    { 
        checkPurge();
    }

    if(state == PURGING) { 
        ledAction(1);
        do_purge();
    }

    if(state == GRINDING_GBW)
    { 
        do_gbw();
        smartPurgeLearnFromMotor(motor_setRPM);
        lastActivity = millis();
    }

    if(state == MENU1)
    { 
        motor_setRPM = 0;
        //everything handled in do_io
    }

    if(state == MENU2) { 
        motor_setRPM = 0;
        //everything handled in do_io
    }

    if(state == MENU3) { 
        motor_setRPM = 0;
        //everything handled in do_io
    }

    if(state == MENU4) {
        motor_setRPM = 0;
    }

    if(state == MENU5) {
        motor_setRPM = 0;
    }

    if(state == CALIBRATING) { 
        ledAction(1);
        Calibrate();
        lastActivity = millis();
    }

    if(state == MOTOR_SELECT) {
        motor_setRPM = 0;
    }

    if(state == NETWORK_INFO) {
        motor_setRPM = 0;
    }

    if(state == ABOUT_INFO) {
        motor_setRPM = 0;
    }

    pollMotorStartWatchdog();
    smart_purge_learning_service_persist(controllerCanIdleSleep(), lastActivity);

   // newData = false;
}


void checkPurge() { 
    //if last call was +1000ms ago, reset.
    if(purgeLastCheckAt + 500 < millis()) { 
        purgeLastCheckAt = millis();
        purgeStartTime = millis();
        ready_purge = false;
        purgeFrameCounter = 0;
    }

    if(commStatus != COMM_CONNECTED) { 
        state = IDLE;
        motor_setRPM = 0;
    } else motor_setRPM = setRPM;

    //Reset if the RPM is changed
    if(setRPM != purgeLastSetRPM) {
        purgeLastSetRPM = setRPM;
        purgeFrameCounter = 0;
        ready_purge = false;
        purgeStartTime = millis();
    }

    // Auto off after x time no grinding detected
    if(Menu2[AUTO_OFF_TIME].value > 0) {
        if(purgeStartTime + Menu2[AUTO_OFF_TIME].value*1000 < millis() && ready_purge == false) { 
            motor_setRPM = 0;
            purge_cancel_pending();
            state = IDLE;
            lastActivity = millis();
        }
    }

    //Auto purge sequence
    if(Menu2[AUTO_PURGE_ENABLED].value == true && purgeStartTime + Menu2[SETPURGESTABILTIME].value < millis() && lastActivity + 1000 < millis() && newData == true) 
    { 

      if(setRPM > 500) { //High values
        purgeTestScalar = Menu2[SETPURGEPRCTHIGH].value;
        purgeFrames = Menu2[SETPURGEFRAMESHIGH].value;
      } else {          // low values
        purgeTestScalar = Menu2[SETPURGEPRCTLOW].value;
        purgeFrames = Menu2[SETPURGEFRAMESLOW].value;
      }

      const uint16_t baselineTorque = purgeBaselineTorqueForRpm(setRPM);
      purgeTestTorque = baselineTorque;
      const uint16_t manualThreshold = purgeManualThresholdForRpm(setRPM, baselineTorque);
      smart_purge_learning_note_sample(setRPM,
                                       motor_currentRPM,
                                       motor_currentTorque,
                                       baselineTorque,
                                       manualThreshold);
      const uint16_t effectiveThreshold = smart_purge_learning_threshold(setRPM,
                                                                         baselineTorque,
                                                                         manualThreshold);
      #ifdef DEBUG_CALIB
        Serial.print("torque: "), Serial.print(motor_currentTorque);
        Serial.print(" base: "), Serial.print(purgeTestTorque);
        Serial.print(" manual: "), Serial.print(manualThreshold);
        Serial.print(" smart: "), Serial.println(effectiveThreshold);
      #endif

      ///if the actual torque is greater than the calibrated torque*scalar we count up frames (note only when newData = true)
      if(motor_currentTorque > effectiveThreshold) {
        purgeFrameCounter++;
        #ifdef DEBUG_CALIB
          Serial.print("frame counter: "), Serial.println(purgeFrameCounter);
        #endif
      } else {  //If it's smaller, we set the frames to 0;
        purgeFrameCounter = 0;
      }

      // If we've made the threshold once, purge will commence after grinding. LastActivity stay millis() as long as we're grinding beans
      if(purgeFrameCounter >= purgeFrames)  
      { 
        ready_purge = true;
        lastActivity = millis(); //Update the last known time we were grinding.. 
      }
      
      //If the frames have gone to 0; we are no longer grinding -> after purgedelay(7) we will commense the purge
      if(ready_purge == true && (lastActivity + Menu2[SETPURGEDELAY].value) < millis()) 
      { 
        #ifdef DEBUG_CALIB
        Serial.println("go purge");
        #endif
        disp_updateRequired = true;
        purge_mark_started();
        state = PURGING;
        }
    }
    purgeLastCheckAt = millis();
}

void do_purge() { 
    uint16_t delayTime = Menu2[SETPURGEOFFTIME].value;

    if(purgeSequenceNeedsInit) { 
        purgeRunStartedAt = millis(); 
        purgePhase = 0;
        purgeLastPhase = 0xFF;
        purgeSequenceNeedsInit = false;
        #ifdef DEBUG
            Serial.println("Purge commence..");
        #endif
        if(Menu2[SETPURGERPM].value == 0 && Menu2[SETPURGEREVERSE].value == 0) {
            purge_cancel_pending();
            state = IDLE;
            return;
        }
    }
    
    //started <delayTime ago
    if(purgeRunStartedAt + delayTime > millis()) { 
        if(delayTime > 0) motor_setRPM = 0; 
        purgePhase = 1; 
    }

    // Time between delayTime and 500 + purgeTime 
    if(purgeRunStartedAt + delayTime < millis() && purgeRunStartedAt + delayTime + Menu2[SETPURGETIME].value > millis()) {
        motor_setRPM = Menu2[SETPURGERPM].value > Menu1[SETMAX].value? Menu1[SETMAX].value : Menu2[SETPURGERPM].value; 
        purgePhase = 2;
    }

    //We're not doing reverse
    if(Menu2[SETPURGEREVERSE].value == false && purgeRunStartedAt + delayTime + Menu2[SETPURGETIME].value < millis()) {
        motor_setRPM = 0;
        purge_cancel_pending();
        state = IDLE;
        disp_updateRequired = true;
        purgePhase = 5;
    } else { 
        //were doing reverse, pause for 500ms 
        if(purgeRunStartedAt + delayTime + Menu2[SETPURGETIME].value < millis() && purgeRunStartedAt + 2*delayTime + Menu2[SETPURGETIME].value > millis()) { 
            motor_setRPM = 0;
            purgePhase = 3;
        }
        //do another reverse purge for purge time
        if(purgeRunStartedAt + 2*delayTime + Menu2[SETPURGETIME].value < millis() && purgeRunStartedAt + 2*delayTime + 2*Menu2[SETPURGETIME].value > millis()) {
            motor_setRPM = (-1) * (Menu2[SETPURGEREVERSE].value > Menu1[SETMAX].value? Menu1[SETMAX].value : Menu2[SETPURGEREVERSE].value);
            purgePhase = 4;
        }
        if(purgeRunStartedAt + 2*delayTime + 2*Menu2[SETPURGETIME].value < millis()) { 
            purgePhase = 5;
            motor_setRPM = 0;
            purge_cancel_pending();
            state = IDLE;
            disp_updateRequired = true;
        }
    }

    if(purgePhase != purgeLastPhase) { 
        Serial.println(purgePhase);
        disp_updateRequired = true;
        purgeLastPhase = purgePhase;
        Serial.println(motor_setRPM);
    }
}


void vitals_init() { 
  esp_err_t status = esp_task_wdt_status(NULL);
  if(status == ESP_OK) {
      esp_task_wdt_delete(NULL);
  }
}

bool controller_idle_sleep_active() {
    return idleSleepActive;
}

void controller_enter_idle_sleep() {
    if(kDisableIdleSleepForTesting || idleSleepActive || !controllerCanIdleSleep()) {
        return;
    }

    idleSleepActive = true;
    ledAction(0);
    display_off();
    disp_updateRequired = false;

    #ifdef DEBUG
    Serial.println("[sleep] idle display sleep active");
    #endif
}

void controller_exit_idle_sleep() {
    if(!idleSleepActive) {
        return;
    }

    idleSleepActive = false;
    display_on();
    disp_updateRequired = true;

    #ifdef DEBUG
    Serial.println("[sleep] idle display sleep cleared");
    #endif
}

void controller_note_activity() {
    lastActivity = millis();
    if(idleSleepActive) {
        controller_exit_idle_sleep();
    }
    disp_updateRequired = true;
}


void sleep_init() { 
    const ControllerBoardPins& pins = controller_board_pins();
    if(!controllerHasLocalDeepSleepWake()) {
    #ifdef DEBUG
    Serial.println("[sleep] no local wake button on this target; restarting instead");
    #endif
    ESP.restart();
    return;
    }

    delay(100); //Allow shit to pass
    if(controller_board_pin_valid(pins.rs485Re)) {
        digitalWrite(pins.rs485Re, HIGH); // Low power state
    }
    if(controller_board_pin_valid(pins.rs485De)) {
        digitalWrite(pins.rs485De, LOW);
    }
    if(controller_board_has_display() && controller_board_pin_valid(pins.displayBacklight)) {
        digitalWrite(pins.displayBacklight, LOW);
    }
    delay(1);

    // Set BTN as input with pullup
    //pinMode(BTN, INPUT_PULLUP);

    rtc_bootFlag1 = true;

    // --- Enable RTC GPIO hold for RE, DE, DISP_BL ---
    if(controller_board_gpio_valid(pins.sleepRe)) {
        rtc_gpio_hold_en(pins.sleepRe);
    }
    if(controller_board_gpio_valid(pins.sleepDe)) {
        rtc_gpio_hold_en(pins.sleepDe);
    }
    if(controller_board_has_display() && controller_board_pin_valid(pins.displayBacklight)) {
        rtc_gpio_hold_en(static_cast<gpio_num_t>(pins.displayBacklight));
    }

    rtc_gpio_hold_dis(pins.wakeButton);
    rtc_gpio_init(pins.wakeButton);
    rtc_gpio_set_direction(pins.wakeButton, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(pins.wakeButton);
    rtc_gpio_pulldown_dis(pins.wakeButton);

    // Configure EXT0 wakeup on BTN, wake on LOW level (button press)
    esp_sleep_enable_ext0_wakeup(pins.wakeButton, 0); // 0 = wake on LOW

    #ifdef DEBUG
        Serial.println("Entering deep sleep, waiting for BTN press to wake up...");
        Serial.print("BTN: "); Serial.println(digitalRead(pins.startButton));
    #endif

    // Flush serial before sleeping
    Serial.flush();

    // Enter deep sleep (will not return until reset/wakeup)
    esp_deep_sleep_start();
}

void sleep_release() { 
    const ControllerBoardPins& pins = controller_board_pins();
   // --- Release RTC GPIO hold for RE, DE, DISP_BL ---
    if(controller_board_gpio_valid(pins.sleepRe)) {
        rtc_gpio_hold_dis(pins.sleepRe);
    }
    if(controller_board_gpio_valid(pins.sleepDe)) {
        rtc_gpio_hold_dis(pins.sleepDe);
    }
    if(controller_board_has_display() && controller_board_pin_valid(pins.displayBacklight)) {
        rtc_gpio_hold_dis(static_cast<gpio_num_t>(pins.displayBacklight));
    }
    if(controller_board_gpio_valid(pins.wakeButton)) {
        rtc_gpio_deinit(pins.wakeButton);
    }

    // Disable EXT0 wakeup source (optional, for clarity)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    rtc_bootFlag1 = false;
}


void doVitals() { 
    #ifdef DEBUG
    unsigned long currentMicros = micros(); // or use millis() for ms accuracy
    unsigned long looptime = currentMicros - lastLoopTime;
    lastLoopTime = currentMicros;

    if(looptime > 500) {
        Serial.print("Loop time: ");
        Serial.print(looptime);
        Serial.println(" us");
    }
    #endif
}


void sleepTimeCheck() { 
    if(kDisableIdleSleepForTesting) {
        if(idleSleepActive) {
            controller_exit_idle_sleep();
        }
        return;
    }
    if(radio_mode_is_off()) {
        if(idleSleepActive) {
            controller_exit_idle_sleep();
        }
        if(controllerRadioOffDeepSleepDue()) {
            rtc_state = state;
            motor_setRPM = 0;
            motorOff();
            motorSleep();
            Serial.println("[sleep] radio off; entering deep sleep");
            sleep_init();
        }
        return;
    }
    if(idleSleepActive) {
        if(Menu1[SETSLEEP].value == 0 || !controllerCanIdleSleep()) {
            controller_exit_idle_sleep();
        }
        return;
    }

    if(Menu1[SETSLEEP].value == 0) return;
    if(!controllerCanIdleSleep()) return;

    if(lastActivity + (Menu1[SETSLEEP].value * 60000) < millis() && millis() > (Menu1[SETSLEEP].value * 60000)) { 
        rtc_state = state; // save last state
        controller_enter_idle_sleep();
        #ifdef DEBUG
            Serial.println("sleep!");
        #endif
    }
}
