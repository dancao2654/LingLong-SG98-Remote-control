#include "gbw.h"
#include "main.h"
#include "io.h"
#include "pdata.h"
#include "display.h"
#include "motor_hal.h"
#include "ble_console.h"

// #define DEBUG_GBW
unsigned long startOfShot = 0;
float speedModifier = 1;
bool slow_phase = false;

//shared between tasks!
volatile int32_t currentWeight = 0; //in MG
volatile bool scaleConnected = false;
volatile SCALE_STATUS scaleStatus = INVALID_SCALE_STATUS;
volatile int16_t newWeightAvail;
//Only used in scaletask:
bool connect = false;
bool maintain = true;

//Used in main loop:
uint16_t slow_phase_at = 0;
bool grindingComplete = false;
bool gbw_started = false;
unsigned long shotStopped = 0;

namespace {
int16_t clamp_gbw_rpm(int16_t rpm) {
    if(rpm < 0) return 0;
    if(rpm > Menu1[SETMAX].value) return Menu1[SETMAX].value;
    return rpm;
}
}


// MUTEX >>
SemaphoreHandle_t scaleMutex;
String scale_connect_mac = "";
String scale_connect_name = "";
String scale_mac = "";
String scale_name = "";
// MUTEX <<

String mac_local = "";

BLEScale scale(DEBUG_SCALE);

GBW_WEIGHT _shot[300];
uint16_t last_shot_updated;

//BLEService weightService("0x0FFE"); // create service
//BLEByteCharacteristic weightCharacteristic("0xFF11",  BLEWrite | BLERead);

//SEPARATE TASK!!
void scales_init() { 
    NimBLEDevice::init("GbWService");
    ble_console_init();
    if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
        mac_local = scale_connect_mac;
        xSemaphoreGive(scaleMutex);
    } 
}

//SEPARATE TASK!!
void gbwVitals() 
{   
    static uint8_t prevScaleStatus;
    static unsigned long lastConnected;
    static int32_t prevWeight;
    const bool scaleModeActive = (state == IDLE_GBW || state == GRINDING_GBW);

    connect = scaleModeActive;
    maintain = scaleModeActive;
    ble_console_set_advertising_enabled(!scaleModeActive);

    if(scale.manage(connect, maintain, mac_local) == true) { 
        scaleStatus = SCALE_CONNECTED;
        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            scale_mac = scale.connected_mac;
            scale_name = scale.connected_name;
            xSemaphoreGive(scaleMutex);
        } 
    }

    if(scale.isConnected() == true) { 
        scaleStatus = SCALE_CONNECTED;

    } else { 
        if(scale.isConnecting() == true) scaleStatus = SCALE_CONNECTING;
        else { 
            scaleStatus = SCALE_DISCONNECTED;
            currentWeight = 0;
        }
    }

    static unsigned long lastCheck;
    if(lastCheck + 5000 < millis()) { 
        lastCheck = millis();

        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            mac_local = scale_connect_mac;
            xSemaphoreGive(scaleMutex);
        }
    }

    // always call newWeightAvailable to actually receive the datapoint from the scale,
    // otherwise getWeight() will return stale data
    if(scale.newWeightAvailable())
    {   
        currentWeight = scale.getWeight() * 1000; // we only deal in mg
        newWeightAvail = true;
        disp_updateRequired = true;
    } else newWeightAvail = false; //Here coz thats one round!

    if(scaleStatus != prevScaleStatus) { 
        disp_updateRequired = true;
        prevScaleStatus = scaleStatus;
    }

    if(prevWeight != currentWeight) { 
        disp_updateRequired = true;
        prevWeight = currentWeight;
    }

}

void disconnect_for_sleep() { 
    maintain = false;
    connect = false;
    scale.disconnect();
}

void do_gbw() { 
    static unsigned long lastCall;
    static unsigned long lastUpdate;
    static unsigned long tareTime;
    static bool properShot;
    static bool learned;

    #ifndef DEBUG_GBW
    if(commStatus != COMM_CONNECTED) { 
        motorOff();
        state = IDLE_GBW;
    }
    #endif

    //reset route & start route
    if(lastCall + 1000 < millis()) { //reset after last call 1000ms ago
        if(motor_setRPM != 0) motor_setRPM = 0;
        //Set starting values properly
        gbw_started = false;
        grindingComplete = false;
        slow_phase = false;
        startOfShot = 0;
        last_shot_updated = 0;
        slow_phase_at = 0;
        lastUpdate = millis();
        shotStopped = 0;
        properShot = false;
        learned = false;

        //Zero shot data
        for(int i = 0; i < sizeof(_shot)/sizeof(_shot[0]); i++) { 
            _shot[i].weight = 0;
            _shot[i].time = 0;
        }

        //Get outta here if scale is not connected;
        if(scaleStatus != SCALE_CONNECTED) { 
            state = IDLE_GBW;
            error = 106;
            disp_updateRequired = true;
            #ifdef DEBUG_GBW
                Serial.println("Scale not connected!"); 
            #endif
            return;
        }

        //check tare
        if(abs(currentWeight) > 100) { // we accept 100mg of deviation.. this is natural for the scale..
            scale.tare();
            tareTime = millis();
            gbw_started = false;
            #ifdef DEBUG_GBW
                Serial.println("Tare");
            #endif

        } else { //no tare needed
            startOfShot = millis();
            last_shot_updated = 0;
            grindingComplete = false;
            gbw_started = true; //coffee imminent
            tareTime = millis(); //for safety?
            #ifdef DEBUG_GBW
                Serial.println("gbw started!");
            #endif
            disp_updateRequired = true;
        }
    }

//keep track of our shot
static int32_t lastWeight;
    if(grindingComplete == false) {
        if(state == GRINDING_GBW && gbw_started == true && lastWeight != currentWeight) { 
            lastWeight = currentWeight;
            last_shot_updated++;
            _shot[last_shot_updated].weight = abs(currentWeight);
            _shot[last_shot_updated].time = millis() - startOfShot;
            if(last_shot_updated >= sizeof(_shot)/sizeof(_shot[0])) last_shot_updated = 0;
        }
    } else { 
        if(lastUpdate + 600 > millis()) { 
            if(state == GRINDING_GBW && gbw_started == true && lastWeight != currentWeight) { 
                lastWeight = currentWeight;
                last_shot_updated++;
                _shot[last_shot_updated].weight = abs(currentWeight);
                _shot[last_shot_updated].time = millis() - startOfShot;
                if(last_shot_updated >= sizeof(_shot)/sizeof(_shot[0])) last_shot_updated = 0;
            }
        }
    }

    lastCall = millis();

    //Tare route
    if(gbw_started == false && tareTime + 2000 < millis()) {
        if(abs(currentWeight) < 200) { 
            startOfShot = millis();
            last_shot_updated = 0;
            grindingComplete = false;
            gbw_started = true;
            disp_updateRequired = true;
            #ifdef DEBUG_GBW
                Serial.println("gbw started!");
            #endif
            lastUpdate = millis();
        } else { //TARE AGAIN IF NEEDED
            scale.tare();
            tareTime = millis();
            gbw_started = false;
            #ifdef DEBUG_GBW
                Serial.println("Tare");
            #endif
        }
    }

    //Actually grinding! Menu5 = button swing delay
    if(gbw_started == true && startOfShot + Menu3[GBW_BUTTON_DELAY].value < millis() && grindingComplete == false && slow_phase == false) { 
        motor_setRPM = clamp_gbw_rpm(Menu3[GBW_RPM_SET].value); //This is the GBW set RPM in menu
    }

    if(Menu3[GBW_SLOW_MG].value > 0) {
        // Switch to slow phase if applicable
        if(!slow_phase && (abs(currentWeight)) >= (setWeight - Menu3[GBW_SLOW_MG].value) && grindingComplete == false && gbw_started == true) { 
            motor_setRPM = clamp_gbw_rpm(min(Menu3[GBW_SLOW_RPM].value, Menu3[GBW_RPM_SET].value));
            slow_phase = true;
            slow_phase_at = millis() - startOfShot; 
            #ifdef DEBUG_GBW
                Serial.print("slow phase..at: "), Serial.print(currentWeight), Serial.println("mg");
            #endif
        }
    }

    //Completion route
    if(gbw_predict() < 0 && grindingComplete == false && gbw_started == true) { 
        grindingComplete = true;
        motor_setRPM = 0;
        lastUpdate = millis();
        shotStopped = millis() - startOfShot;
        #ifdef DEBUG_GBW
            Serial.println("Grinding complete!");
            Serial.print("weight: "), Serial.print(abs(currentWeight)), Serial.print(" time: "), Serial.println(millis()-startOfShot);
        #endif
        disp_updateRequired = true;
        properShot = true;
    }

    //Backup completion route
    if(abs(currentWeight) > setWeight + 1000 && motor_setRPM > 0 && startOfShot > 1000) { 
        grindingComplete = true;
        motor_setRPM = 0;
        lastUpdate = millis();
        shotStopped = millis() - startOfShot;
        #ifdef DEBUG_GBW
            Serial.println("Backup completion triggered.. bad!");
            Serial.println(currentWeight);
            Serial.println(setWeight);
        #endif
        properShot = false;
        disp_updateRequired = true;
    }

    //Scale issues route
    if(scale.isConnected() == false && grindingComplete == false) { 
        #ifdef DEBUG_GBW
            Serial.println("Scale issues..");
        #endif
        motor_setRPM = 0;
        grindingComplete = true;
        gbw_started = false;
        state = IDLE_GBW;
        error = 104;
        disp_updateRequired = true;
        properShot = false;
    } 

    //No grounds coming out after 3s, no beans?
    if(startOfShot + 3000 < millis() && grindingComplete == false && gbw_started == true && abs(currentWeight) < 200) { 
        motor_setRPM = 0;
        grindingComplete = false;
        gbw_started = false;
        slow_phase = false;
        properShot = false;
        state = IDLE_GBW;
        error = 105;
        disp_updateRequired = true;
    }

    // Learn route
    if(grindingComplete == true && lastUpdate + 1000 < millis() && learned == false) { 
        scale.startTimer(); //BEEP!
        delay(25);
        scale.stopTimer(); 
        if(motor_setRPM != 0) motorOff();
        #ifdef DEBUG_GBW
            Serial.println("learn!");
        #endif
        if(properShot) gbw_learn(); //only needs to run once
        #ifdef DEBUG_GBW
            Serial.println("return from learn..!");
        #endif
        learned = true;
        lastActivity = millis();
    }

    if(grindingComplete == true && lastUpdate + 2000 < millis()) { 
        state = IDLE_GBW;
        disp_updateRequired = true;
        grindingComplete = false;
        gbw_started = false;
        tareTime = 0;
        slow_phase = false;
        slow_phase_at = 0;
        properShot = false;
    }

    // flash accordingly
    if(gbw_started == false) ledAction(0);
    if(gbw_started == true && grindingComplete == false) ledAction(100 + 50*(float(currentWeight)/float(setWeight)));
    else if(grindingComplete == true) ledAction(1);
}


static float gbw_liveRateMgPerMs(uint16_t windowSize = 6, uint16_t reference = last_shot_updated) {
    uint16_t oldIdx;
    if (reference > last_shot_updated) reference = last_shot_updated;

    if(last_shot_updated <= windowSize) { 
        oldIdx = 0;
    } else { 
        oldIdx = reference - windowSize;
    }

    uint16_t numPoints = reference - oldIdx;
    if (numPoints < 2) return -1;

    // Linear regression: fit y = mx + b through all points
    // where x = time, y = weight
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    
    for (uint16_t i = oldIdx; i < reference; i++) {
        float x = _shot[i].time;
        float y = _shot[i].weight;
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }
    
    float n = numPoints;
    float denom = (n * sumX2 - sumX * sumX);
    
    if (denom == 0) return 0;  // All points identical in time
    
    float slope = (n * sumXY - sumX * sumY) / denom;
    if(slope > 20) slope = 20; //simple solution to noisy data at the start

    return slope;  // mg per ms
}

int32_t gbw_predict() {
    int32_t predictedTime = 5000;
    static int32_t lastKnownWeight;
    static unsigned long lastCall;
    static int32_t lastActualPrediction;
    static unsigned long lastPredictionAt;
    speedModifier = Menu3[GBW_SPEEDMOD].value / 10000.0f;

    if(lastCall + 1000 < millis()) { 
        lastActualPrediction = 5000;
        lastPredictionAt = millis();
        predictedTime = 5000;
    }

    lastCall = millis();

    if(currentWeight != lastKnownWeight && gbw_started && !grindingComplete) {
        lastKnownWeight = currentWeight;
        if(state == GRINDING_GBW && gbw_started == true) {  
            // Use the most recent shot samples to estimate current grind speed.
            float speed = gbw_liveRateMgPerMs(6, last_shot_updated);
            float stillToGrind = float(setWeight) - float(abs(currentWeight));

            if(speed > 0.0f && speed < 20.0f) {
                predictedTime = (int32_t)(stillToGrind / speed);
            } else { 
                predictedTime =  (int32_t)(stillToGrind / ((Menu3[GBW_SPEEDMOD].value/10000.f) * (clamp_gbw_rpm(Menu3[GBW_RPM_SET].value) / 60.f)));
            }

            lastPredictionAt = millis();
            lastActualPrediction = predictedTime;

            #ifdef DEBUG_GBW
                Serial.print("New prediction: "), Serial.print(predictedTime), Serial.print(" ..at: "), Serial.print(lastPredictionAt - startOfShot), Serial.print("ms...mg: "),Serial.print(currentWeight), Serial.print("  to go:  "), Serial.print(stillToGrind), Serial.print(" speed: "), Serial.println(speed);
            #endif
        } 
    }

    //Give a continuous prediction value
    predictedTime = lastActualPrediction - (millis() - lastPredictionAt); 

    if(gbw_started == true && grindingComplete == true) predictedTime = 0;
    return (int32_t)(predictedTime - abs(Menu3[GBW_OFFSET].value));
}


void gbw_learn() { 
    int overshoot = int(currentWeight) - int(setWeight);
    uint32_t stopWeight;
    float stopSpeed;
    bool isProper = true;
    int16_t newOffset = Menu3[GBW_OFFSET].value;
    uint16_t firstVal = 0;
    uint16_t secondVal = 0;


    //estimate weight at stoptime
    for(int i = 0; i < last_shot_updated; i++) { 
        if(_shot[i].time > shotStopped) {
            uint16_t delta = _shot[i].time - _shot[i-1].time;
            uint16_t point = shotStopped - _shot[i-1].time;
            float progression = float(point) / (float)delta;

            #ifdef DEBUG_GBW
                Serial.print("delta .. point .. progression: "), Serial.print(delta), Serial.print(" "), Serial.print(point), Serial.print(" "), Serial.println(progression);
            #endif
            
            stopWeight = _shot[i-1].weight + progression * (_shot[i].weight - _shot[i-1].weight);
            stopSpeed = gbw_liveRateMgPerMs(4, i);

            break;
        }
    }

    #ifdef DEBUG_GBW
        Serial.print("last shot updated idx: "), Serial.println(last_shot_updated);
        Serial.print("final weight: "), Serial.println(currentWeight);
        Serial.print("target weight: "), Serial.println(setWeight);
        Serial.print("grinding duration: "), Serial.print(shotStopped), Serial.println("ms");
        Serial.print("grinding stopped estimation: "), Serial.print(stopWeight), Serial.println("mg");
        Serial.print("speed at stopping mg/ms: "), Serial.println(stopSpeed);
        Serial.print("overshoot: "), Serial.println(overshoot);

        Serial.println(""), Serial.println("datapoints:");
        for(int i = 0; i < last_shot_updated; i++) { 
            Serial.print("idx: ");
            Serial.print(i);
            Serial.print("  weight: ");
            Serial.print(_shot[i].weight);
            Serial.print(" time: ");
            Serial.println(_shot[i].time);
            if(_shot[i].time < shotStopped && _shot[i+1].time > shotStopped) Serial.println("-- END OF GRINDING --");
        }
    #endif
    
    // Lookup the BULK grinding speed with time between 2 and 10g 
    for(int i = 0; i < last_shot_updated; ++i) {
        if(_shot[i].weight > 2000 && firstVal == 0) {
            firstVal = i;
        }
        if(_shot[i].weight > 10000 && secondVal == 0) {
            secondVal = i;
        }
        if(firstVal != 0 && secondVal != 0) {
            break;
        }
    }

    int16_t test_weight_diff = _shot[secondVal].weight - _shot[firstVal].weight;
    uint32_t test_time_diff = _shot[secondVal].time - _shot[firstVal].time;

    
 
    float grindSpeed = test_weight_diff / float(test_time_diff);
    float rpm = clamp_gbw_rpm(Menu3[GBW_RPM_SET].value);

    if (rpm == 0) return; // Prevent division by zero
    float ground_per_rotation = grindSpeed / (rpm / 60.f); // g/s / rot/s = g * rot

    if(speedModifier != 0.5f) speedModifier = (ground_per_rotation + speedModifier) / 2.0f;
    else speedModifier = ground_per_rotation;


    #ifdef DEBUG_GBW
        Serial.print("Old speedmodifier: ");
        Serial.print(Menu3[GBW_SPEEDMOD].value);
        Serial.print("  New speedmod: ");
        Serial.println(speedModifier*10000);
    #endif

    // Calculating expected stop time
    if(Menu3[GBW_OFFSET].value <= 0) {
        return;
    }
    int16_t actualDeltaWeight = _shot[last_shot_updated].weight - stopWeight; 
    float endingSpeed = float(actualDeltaWeight) / float(Menu3[GBW_OFFSET].value);
    int16_t grindOvershoot = setWeight - stopWeight; 

    int16_t idealOffset = (actualDeltaWeight / endingSpeed) + (overshoot/endingSpeed)/4.f;

    if(overshoot > 0 && idealOffset > Menu3[GBW_OFFSET].value) newOffset = (Menu3[GBW_OFFSET].value + idealOffset)/2;
    if(overshoot < 0 && idealOffset < Menu3[GBW_OFFSET].value) newOffset = (Menu3[GBW_OFFSET].value + idealOffset)/2;
    if(abs(overshoot) < 100) newOffset = Menu3[GBW_OFFSET].value;

    //Decide if proper!
    if(idealOffset < 50 || idealOffset > 500) isProper = false;
    if(abs(overshoot) > 5000) isProper = false;
    if (test_time_diff < 600) isProper = false; // It's not a proper shot.
    if(firstVal == 0 || secondVal == 0) isProper = false; //not a proper shot
    if (test_time_diff == 0) isProper = false; // Prevent division by zero
    if(speedModifier < 0.03f || speedModifier > 0.8f) isProper = false;
 

    #ifdef DEBUG_GBW
        Serial.print("Actual delta weight: "), Serial.println(actualDeltaWeight);
        Serial.print("endingSpeed: "), Serial.println(endingSpeed);
        Serial.print("live calc ending Speed: "), Serial.println(stopSpeed);
        Serial.print("idealOffset: "), Serial.println(idealOffset);
        Serial.print("last offset: "), Serial.println(Menu3[GBW_OFFSET].value);
    #endif

    if(isProper == true) { 
        #ifdef DEBUG_GBW
            Serial.println("Proper shot! saving values!");
        #endif
        Menu3[GBW_SPEEDMOD].value = speedModifier * 10000;
        Menu3[GBW_OFFSET].value = newOffset;
        pdata_write(4);
    } else { 
        #ifdef DEBUG_GBW
            Serial.println("Not a proper shot.. deleting calculated values..");
            Serial.print("Calculated speedmod: ");
            Serial.println(speedModifier, 3);
            Serial.print("Calculated new Offset: ");
            Serial.println(newOffset);
        #endif
    }
}
