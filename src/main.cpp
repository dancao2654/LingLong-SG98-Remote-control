#include <driver/rtc_io.h>
#include "esp_task_wdt.h"
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

States state = IDLE;
int8_t menu1Selected = EXITMENU;
int8_t menu2Selected = RETURN_FROM_PURGE;
int8_t menu3Selected = RETURN_FROM_GBW;
int8_t menu4Selected = RETURN_FROM_JMC_BAUD;
int8_t motorSelectSelected = MOTOR_SELECT_AUTO;
bool enterMenu = false;
bool firstBootFlag = false;
States motorSelectReturnState = IDLE;

menuEntry Menu1[NUM_MENU1_ITEMS] = {
    {"EXIT MENU", 0, 0, 0, 0, "EXITMENU", ""},                 // 0
    {"Purge Settings", 0, 0, 0, 0, "PURGESETT", ""},           // 1
    {"GbW Settings", 0, 0, 0, 0, "GBWSETT", ""},               // 2
    {"JMC Baud", 0, 0, 0, 0, "JMCBAUDMENU", ""},               // 3
    {"Sleep time", default_sleepTime, 0, 300, 5, "SLEEPT", "min"},// 4
    {"Brightness", default_brightness, 1, 255, 1, "BRIGHTNESS", ""}, // 5
    {"LED max Brightness", default_led_brightness, 0, 100, 1, "LEDPRCT", "%"}, // 6
    {"Invert scrolling", 0, 0, 1, 1, "SCROLL", ""},            // 7
    {"Max RPM", default_maxRPM, absolute_min_rpm, absolute_max_rpm, rpm_scalar, "MAXRPM", "rpm"}, // 8
    {"Min RPM", default_minRPM, absolute_min_rpm, 500, rpm_scalar, "MINRPM", "rpm"},              // 9
    {"Motor torque", default_motor_torque, 1, 300, 1, "MOTORTORQ", "%"},               // 10
    {"Motor ramp", default_motor_ramp, 0, 12, 1, "MOTORRMP", "lvl"}, //11
    {"Calibrate", 0, 0 ,0, 0, "CALIBRATE", ""},           // 12
    {"Reset Network", 0, 0, 1, 1, "RSTNET", ""},          // 13
    {"Reset default", 0, 0, 1, 1, "RSTALL", ""},          // 14
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
}

void purge_prepare_for_grinding() {
    purgeFrameCounter = 0;
    purgeLastSetRPM = setRPM;
    purgeStartTime = millis();
    purgeLastCheckAt = millis();
    ready_purge = false;
}

void purge_cancel_pending() {
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
    Serial.print("Wakeup cause: ");
    Serial.println(esp_sleep_get_wakeup_cause());
    Serial.print("Reset reason: ");
    Serial.println(esp_reset_reason());
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
    wifi_ui_init();
    vitals_init();
    lastActivity = millis();
}

void loop() { 
    doVitals();
    do_comm();
    diag_console_poll();
    wifi_ui_poll();
    do_io();
    sleepTimeCheck();
    update_display();
    

    if(state == SLEEPING){ 
        esp_task_wdt_deinit();
        ledAction(0);
        motorOff();
        display_off();
        motorSleep();
        sleep_init();
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

    if(state == CALIBRATING) { 
        ledAction(1);
        Calibrate();
        lastActivity = millis();
    }

    if(state == MOTOR_SELECT) {
        motor_setRPM = 0;
    }

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

    //the torque defined during calibration
      if(rpm_scalar != 0) purgeTestTorque = calibrateArray[((setRPM + absolute_min_rpm)/rpm_scalar)];
      #ifdef DEBUG_CALIB
        Serial.print("torque: "), Serial.print(motor_currentTorque), Serial.print(" test: "), Serial.println(purgeTestTorque);
      #endif
      if(setRPM > 500) { //High values
        purgeTestScalar = Menu2[SETPURGEPRCTHIGH].value;
        purgeFrames = Menu2[SETPURGEFRAMESHIGH].value;
      } else {          // low values
        purgeTestScalar = Menu2[SETPURGEPRCTLOW].value;
        purgeFrames = Menu2[SETPURGEFRAMESLOW].value;
      }

      ///if the actual torque is greater than the calibrated torque*scalar we count up frames (note only when newData = true)
      if(motor_currentTorque > (purgeTestTorque*purgeTestScalar)/100.f) { 
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
  esp_task_wdt_init(3, false);
  esp_task_wdt_add(NULL); // Add current task (loop task)
}


void sleep_init() { 
    delay(100); //Allow shit to pass
    digitalWrite(RS485RE, HIGH); // Low power state
    digitalWrite(RS485DE, LOW); 
    digitalWrite(DISP_BL, LOW);
    delay(1);

    // Set BTN as input with pullup
    //pinMode(BTN, INPUT_PULLUP);

    rtc_bootFlag1 = true;

    // --- Enable RTC GPIO hold for RE, DE, DISP_BL ---
    rtc_gpio_hold_en((gpio_num_t)RS485RE);
    rtc_gpio_hold_en((gpio_num_t)RS485DE);
    rtc_gpio_hold_en((gpio_num_t)DISP_BL);


    rtc_gpio_hold_dis((gpio_num_t)BTN); // Release hold just in case for wake btn
    rtc_gpio_init((gpio_num_t)BTN);
    rtc_gpio_set_direction((gpio_num_t)BTN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)BTN);

    // Configure EXT0 wakeup on BTN (GPIO 4), wake on LOW level (button press)
    esp_sleep_enable_ext0_wakeup(WAKE_BTN, 0); // 0 = wake on LOW

    #ifdef DEBUG
        Serial.println("Entering deep sleep, waiting for BTN press to wake up...");
        Serial.print("BTN: "); Serial.println(digitalRead(BTN));
    #endif

    // Flush serial before sleeping
    Serial.flush();

    // Enter deep sleep (will not return until reset/wakeup)
    esp_deep_sleep_start();
}

void sleep_release() { 
   // --- Release RTC GPIO hold for RE, DE, DISP_BL ---
    rtc_gpio_hold_dis((gpio_num_t)RS485RE);
    rtc_gpio_hold_dis((gpio_num_t)RS485DE);
    rtc_gpio_hold_dis((gpio_num_t)DISP_BL);

    // Disable EXT0 wakeup source (optional, for clarity)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    rtc_bootFlag1 = false;
}


void doVitals() { 
    esp_task_wdt_reset();

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
    if(Menu1[SETSLEEP].value == 0) return;
    if(state == GRINDING || state == GRINDING_GBW) return;

    if(lastActivity + (Menu1[SETSLEEP].value * 60000) < millis() && millis() > (Menu1[SETSLEEP].value * 60000)) { 
        rtc_state = state; // save last state
        state = SLEEPING;
        #ifdef DEBUG
            Serial.println("sleep!");
        #endif
    }
}
