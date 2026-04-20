#include "motorcontrol_rt.h"
#include "main.h"
#include "pdata.h"
#include "comm.h"
#include "display.h"
#include "error_history.h"

namespace {
uint16_t motor_currentStatus = 0;
constexpr unsigned long RT_RPM_LOG_ZERO_DELAY_MS = 2000;
unsigned long rtRpmSessionStartedAt = 0;
bool rtRpmLoggedSample = false;
bool rtRpmLoggedZero = false;

bool rtRpmLogStateActive() {
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

uint32_t packMotorRpmLogValue(uint8_t subtype, uint8_t source, int16_t value) {
    return (static_cast<uint32_t>(subtype) << 24) |
           (static_cast<uint32_t>(source) << 16) |
           static_cast<uint16_t>(value);
}

void resetRtRpmLogSession() {
    rtRpmSessionStartedAt = 0;
    rtRpmLoggedSample = false;
    rtRpmLoggedZero = false;
}

void noteRtRpmRead(int16_t rpm) {
    if(!rtRpmLogStateActive() || motor_setRPM == 0) {
        return;
    }

    if(rtRpmSessionStartedAt == 0) {
        rtRpmSessionStartedAt = millis();
    }

    if(!rtRpmLoggedSample && rpm != 0) {
        error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                          ERROR_HISTORY_LEVEL_INFO,
                          ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                          packMotorRpmLogValue(ERROR_HISTORY_MOTOR_RPM_SAMPLE,
                                               ERROR_HISTORY_MOTOR_RPM_SRC_RT_1301,
                                               rpm),
                          state);
        rtRpmLoggedSample = true;
    }
}

void checkRtRpmZeroLog() {
    if(!rtRpmLogStateActive() || motor_setRPM == 0) {
        resetRtRpmLogSession();
        return;
    }

    if(rtRpmSessionStartedAt == 0) {
        rtRpmSessionStartedAt = millis();
    }

    if(!rtRpmLoggedSample && !rtRpmLoggedZero &&
       millis() - rtRpmSessionStartedAt >= RT_RPM_LOG_ZERO_DELAY_MS &&
       motor_currentRPM == 0) {
        error_history_log(ERROR_HISTORY_SOURCE_MOTOR,
                          ERROR_HISTORY_LEVEL_WARN,
                          ERROR_HISTORY_CODE_MOTOR_RPM_TEST,
                          packMotorRpmLogValue(ERROR_HISTORY_MOTOR_RPM_ZERO,
                                               ERROR_HISTORY_MOTOR_RPM_SRC_RT_1301,
                                               motor_setRPM),
                          state);
        rtRpmLoggedZero = true;
    }
}
}

// Actual register mirrors (reflect what the controller has confirmed)
int16_t reg_201 = -1; //enable disable drive (0 / 1)
int16_t reg_503 = 0; //Set torque;
int16_t reg_514 = -1; //Speed limit forward (pos value)
int16_t reg_515 = -1; //Speed limit reverse (pos value)
int16_t reg_525 = -1; //JOG disable / enable (0 / 1)
int16_t reg_701 = -1;

//Only necessary for sending multiple writes
int16_t pending_reg_514 = 0xFFFF;
int16_t pending_reg_515 = 0xFFFF;

/* Important motor parameters for RT, note this is decimal, and should be communicated in HEX (below is bin)
0022 -> motor rated torque

0100 -> control mode (3 = torque mode)

1300 -> running status
1301 -> current RPM
1303 -> motor torque
1353 -> internal fault code -> no clue what is what ??

*/

void motor_rt_init_backend() {
  motor_setRPM = 0;
  reg_201 = -1;
  reg_503 = 0;
  reg_514 = -1;
  reg_515 = -1;
  reg_525 = -1;
  resetRtRpmLogSession();
  commStatus = COMM_DISCONNECTED;
  currentStatus = MOTOR_NOT_CONNECTED;
}

bool motor_rt_do_comm_backend() { // can be called continously and will update all values. Returns true when new data packet is available.
    /* COMM TYPES
    0 = send RPM
    1+ = READ STATUS, RPM, TORQUE (1300, 1301, 1302, 1303)
    */
  static unsigned long lastCommSend;
  static unsigned long lastCommReceived;
  static uint8_t lastCommType;
  static uint8_t nextCommType;
  static int16_t motor_lastSetRPM;

  //Disconnect flow
  // BLE scale activity can momentarily stretch the loop timing; use the explicit
  // disconnect budget instead of the idle polling interval to avoid false drops.
  if(lastCommReceived > 0 && lastCommReceived + DISCONNECTED_AFTER < millis()) { 
    #ifndef DEBUG
      error = 2;
    #endif
    commStatus = COMM_DISCONNECTED;
    currentStatus = MOTOR_NOT_CONNECTED;
    disp_updateRequired = true;
    reg_201 = -1;
    reg_514 = -1;
    reg_515 = -1;
    reg_525 = -1;
    reg_201 = -1;
  }

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
  int16_t torquePercent;
  if((Menu1[SETMOTORTORQUE].value*10) > 0) torquePercent = (1000 * motor_currentTorque / (Menu1[SETMOTORTORQUE].value*10)); //A value 
  else torquePercent = 0;
  if(motor_setRPM != 0 && torquePercent > 90 && abs(motor_currentRPM) < rpm_scalar) { 
    motor_setRPM = 0;
    state = IDLE; 
    error = 6;
  }

  checkRtRpmZeroLog();

  // First communication when disconnected
  if(commStatus == COMM_DISCONNECTED) { 
    if(writeMultipleConfirm(514, 2, 0, 0)) {
      #ifdef DEBUG_MOTORCONTROL
        Serial.println("We're connected!");
      #endif
      int score = 0;
      Serial.println("701: "), Serial.println(readReturn(701, 1));
      //if(writeConfirm(701, Menu1[SETMOTORRAMP].value)) { reg_701 = Menu1[SETMOTORRAMP].value ; score++; } 

      reg_514 = 0;
      reg_515 = 0;
      reg_525 = -1;
      reg_201 = -1;
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
    if(reg_514 != 0) { 
      if(writeMultipleConfirm(514, 2, 0, 0)) { 
        reg_514 = 0; 
        reg_515 = 0; 
      }
    }
    motor_lastSetRPM = motor_setRPM;
    nextCommType = 0;
    return false;
  }

  // >>>>>>>> RECEIVE <<<<<<<< 
  int16_t received = receive();
  if(received > 0) {
    #ifdef DEBUG_MOTORCONTROL
      Serial.print("received!: "), Serial.println(received);
      Serial.print("Commcounter: "), Serial.println(commCounter);
      Serial.print("next comm type: "), Serial.println(nextCommType); 
    #endif
    // ERROR HANDLING 
    if(error == 2 || error == 1) { 
      error = 0;
      disp_updateRequired = true;
    }

    // DEBUG 
    #ifdef DEBUG_MOTORCONTROL
      if(lastRequestType == 0 || lastRequestType > 200) {
        Serial.println("received succesfull!");
        Serial.print("reg 201: "), Serial.println(reg_201);
        Serial.print("reg 503: "), Serial.println(reg_503);
        Serial.print("reg 514: "), Serial.println(reg_514);
        Serial.print("reg 515: "), Serial.println(reg_515);
        Serial.print("reg 525: "), Serial.println(reg_525);
      }
    #endif

    // STATUS UPDATING
    commStatus = COMM_CONNECTED;
    if(currentStatus == MOTOR_NOT_CONNECTED) currentStatus = MOTOR_NOT_READY;
    lastCommReceived = millis(); 

    // >>>>>> SEND SINGLE WRITE
    if(lastRequestType == 0) {  
      switch(received) {
        case 201: { reg_201 = lastRead[0]; break; }
        case 503: { reg_503 = lastRead[0]; break; }
        case 514: { reg_514 = lastRead[0]; break; }
        case 515: { reg_515 = lastRead[0]; break; }
        case 525: { reg_525 = lastRead[0]; break; }
      }
    }

    // >>>>> DATA REQUESTS (1300 && 0x6041)
    if(lastRequestType > 100 && lastRequestType < 200) { 
      if(received == 1300 && lastRequestType == 104) { // A request for status, speed & torque
          motor_currentStatus = lastRead[0];
          motor_currentRPM = lastRead[1];
          noteRtRpmRead(motor_currentRPM);
          //skip 2 = setRPM
          motor_currentTorque = lastRead[3];
          newData = true;
          #ifdef DEBUG_MOTORCONTROL
            Serial.println("Newdata!");
          #endif
          if(motor_currentStatus == 0) currentStatus = MOTOR_READY;
          if(motor_currentStatus == 1) currentStatus = MOTOR_ENABLED;
      }
    }

    // >>>>> Send multiple writes
    if(lastRequestType > 200 && lastRequestType < 205) { //A SEND of max 4 writes
      if(received == 514 && lastRequestType == 202) { 
        reg_514 = pending_reg_514;
        reg_515 = pending_reg_515;
      }
    } 

    return true;
  } else newData = false; // We've made one complete round!

  //Determine COMM delay
  uint16_t delay;
  if(motor_setRPM == 0) if(reg_201!=0 && reg_514!=0 && reg_515!=0 && reg_525!=0 && nextCommType > 0) nextCommType = 0; //We need to SEND

  if (nextCommType == 0) delay = comm_get_send_delay_ms(); 
  if(nextCommType > 0) { 
    if (state == IDLE || state == IDLE_GBW || state == MENU1 || state == MENU2 || state == MENU3 || state == MENU4) delay = comm_get_idle_delay_ms();
    else delay = comm_get_active_interval_ms();
  }

  if(lastCommSend + delay > millis()) return false;

  // >>>>>> MOTOR OFF
  if(motor_setRPM == 0) { 
      if(reg_514 != 0 || reg_515 != 0) { 
        if(writeMultipleRegisters(514, 2, 0, 0)) { //SET SPEED LIMIT FORWARD = 0
          lastCommSend = millis();
          lastCommType = 0;
          nextCommType = 0;
          pending_reg_514 = 0;
          pending_reg_515 = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_525 != 0) { 
        if(writeSingleRegister(525, 0)) { // JOG DISABLE
          lastCommSend = millis();
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_201 != 0) { 
        if(writeSingleRegister(201, 0)) { //SERVO DISABLE
          lastCommSend = millis(); 
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      
      // Check if we've done all commands.
      if(reg_201 == 0 && reg_514 == 0 && reg_525 == 0 && nextCommType == 0) { 
        if(nextCommType == 0) nextCommType = 1; 
      }
  }

    //>>>>> Forward ROTATION
    if(motor_setRPM > 0) { 
      if(reg_201 != 1) { 
        if(writeSingleRegister(201, 1)) { // Servo ENABLED
          lastCommSend = millis(); 
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_503 != (Menu1[SETMOTORTORQUE].value*10)) { 
        if(writeSingleRegister(503, (Menu1[SETMOTORTORQUE].value*10))) {// MOTOR TORQUE SET
          lastCommSend = millis(); 
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_514 != motor_setRPM || reg_515 != 0) { 
        if(writeMultipleRegisters(514, 2, motor_setRPM, 0)) { //SET SPEED LIMIT FORWARD
          lastCommSend = millis();
          lastCommType = 0;
          nextCommType = 0;
          pending_reg_514 = motor_setRPM;
          pending_reg_515 = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_525 != 1) { 
        if(writeSingleRegister(525, 1)) { // JOG FORWARD
          lastCommSend = millis();
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      //Check if we've done all commands
      if(reg_201 == 1 && reg_503 == (Menu1[SETMOTORTORQUE].value*10) && reg_514 == motor_setRPM && reg_515 == 0 && reg_525 == 1) { 
        if(nextCommType == 0) nextCommType = 1; 
      }
    } 

    // >>>> REVERSE ROTATION
     if(motor_setRPM < 0) { 
      if(reg_201 != 1) { 
        if(writeSingleRegister(201, 1)) { // Servo ENABLED
          lastCommSend = millis(); 
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_503 != -(Menu1[SETMOTORTORQUE].value*10)) { 
        if(writeSingleRegister(503, -(Menu1[SETMOTORTORQUE].value*10))) { // MOTOR TORQUE SET
          lastCommSend = millis(); 
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      if(reg_514 != 0 || reg_515 != -motor_setRPM) { 
        if(writeMultipleRegisters(514, 2, 0, -motor_setRPM)) { //SET SPEED LIMIT REVERSE, note motor_set is signed and sending UNSIGNED
          lastCommSend = millis();
          lastCommType = 0;
          nextCommType = 0;
          pending_reg_514 = 0;
          pending_reg_515 = -motor_setRPM;
          return false;
        }
      }
      if(reg_525 != 2) { 
        if(writeSingleRegister(525, 2)) { // JOG FORWARD
          lastCommSend = millis();
          lastCommType = 0;
          nextCommType = 0;
          motor_lastSetRPM = motor_setRPM;
          return false;
        }
      }
      //Check if we've done all commands
      if(reg_201 == 1 && reg_503 == -(Menu1[SETMOTORTORQUE].value*10) && reg_514 == 0 && reg_515 == -motor_setRPM && reg_525 == 2) { 
        if(nextCommType == 0) nextCommType = 1; 
      }
  }

  //VERIFY PARAMETER SET (DEBUG ONLY)
  #ifdef DEBUG_MOTORCONTROL

  #endif

  // READING
  if(nextCommType > 0 && nextCommType < 10) { 
    // nextCommType++;
    readRegister(1300, 4); //read register 1300, 1301, 1302, 1303
    lastCommSend = millis();
    return false;
  }

  return false;
}

bool motor_rt_off_backend() {
  uint8_t score = 0;
    for(int i = 0; i < 10; i++) {
      if(writeMultipleConfirm(514, 2, 0, 0)) { //SET SPEED LIMIT FORWARD = 0
        reg_514 = 0;
        reg_515 = 0;
        score++;
        break; 
      } 
      delay(200);
    }
    for(int i = 0; i < 10; i++) {
      delay(200);
      if(writeConfirm(525, 0)) { // JOG DISABLE
        reg_525 = 0;
        score++;
        break;
      }
    }
    for(int i = 0; i < 10; i++) {
      delay(200);
      if (writeConfirm(201, 0)) { //SERVO DISABLE
        reg_201 = 0;
        score++;
        break;
      }
    }
    if(score == 3) return true;
    else return false;
    return false;
}

void motor_rt_sleep_backend() {
  // Serial.print("Motor sleep..");
  // if(writeConfirm(0x6040, 0)) { 
  //   Serial.println("confirmed");
  // }
}

//This function needs to be called continuously untill it returns true; which indicates a completed calibration
bool motor_rt_calibrate_backend() { 
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
