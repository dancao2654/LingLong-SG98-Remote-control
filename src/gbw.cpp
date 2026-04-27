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

GBW_WEIGHT _shot[300];
uint16_t last_shot_updated;

namespace {
constexpr int32_t kGbwGoodShotBandMg = 200;
constexpr int32_t kGbwAdjustBandMg = 200;
constexpr int32_t kGbwStrongAdjustBandMg = 400;
constexpr uint8_t kGbwSlowMgConfidence = 2;
constexpr uint8_t kGbwSlowRpmConfidence = 4;
constexpr uint8_t kGbwMaxLearningConfidence = 8;

int32_t gbwErrorTrendMg = 0;
uint8_t gbwLearningConfidence = 0;

int16_t clamp_gbw_rpm(int16_t rpm) {
    if(rpm < 0) return 0;
    if(rpm > Menu1[SETMAX].value) return Menu1[SETMAX].value;
    return rpm;
}

int32_t abs_mg(int32_t value) {
    return value < 0 ? -value : value;
}

int16_t clamp_menu_value(const menuEntry& entry, int32_t value) {
    if(value < entry.minValue) return entry.minValue;
    if(value > entry.maxValue) return entry.maxValue;
    return static_cast<int16_t>(value);
}

int16_t clamp_slow_rpm(int32_t rpm) {
    int32_t maxRpm = Menu3[GBW_SLOW_RPM].maxValue;
    if(maxRpm > Menu3[GBW_RPM_SET].value) maxRpm = Menu3[GBW_RPM_SET].value;
    if(maxRpm > Menu1[SETMAX].value) maxRpm = Menu1[SETMAX].value;
    if(rpm < Menu3[GBW_SLOW_RPM].minValue) return Menu3[GBW_SLOW_RPM].minValue;
    if(rpm > maxRpm) return static_cast<int16_t>(maxRpm);
    return static_cast<int16_t>(rpm);
}

int16_t clamp_enabled_slow_mg(int32_t slowMg) {
    int32_t minSlowMg = Menu3[GBW_SLOW_MG].scalar * 5;
    if(minSlowMg < Menu3[GBW_SLOW_MG].minValue) minSlowMg = Menu3[GBW_SLOW_MG].minValue;
    if(slowMg < minSlowMg) return static_cast<int16_t>(minSlowMg);
    return clamp_menu_value(Menu3[GBW_SLOW_MG], slowMg);
}

uint16_t shot_last_index() {
    const uint16_t maxIndex = (sizeof(_shot) / sizeof(_shot[0])) - 1;
    return last_shot_updated > maxIndex ? maxIndex : last_shot_updated;
}

uint16_t shot_index_at_or_after_weight(int32_t weightMg) {
    const uint16_t last = shot_last_index();
    for(uint16_t i = 1; i <= last; i++) {
        if(_shot[i].weight >= weightMg) return i;
    }
    return 0;
}

uint16_t shot_index_at_or_after_time(uint32_t timeMs) {
    const uint16_t last = shot_last_index();
    for(uint16_t i = 1; i <= last; i++) {
        if(_shot[i].time >= timeMs) return i;
    }
    return last;
}

int32_t shot_weight_at_time(uint32_t timeMs, bool* ok = nullptr) {
    const uint16_t last = shot_last_index();
    if(ok) *ok = false;
    if(last == 0) return 0;

    for(uint16_t i = 1; i <= last; i++) {
        if(_shot[i].time == timeMs) {
            if(ok) *ok = true;
            return _shot[i].weight;
        }

        if(_shot[i].time > timeMs) {
            if(i == 1) return _shot[i].weight;

            const uint32_t prevTime = _shot[i - 1].time;
            const uint32_t deltaTime = _shot[i].time - prevTime;
            if(deltaTime == 0) return _shot[i].weight;

            const uint32_t point = timeMs - prevTime;
            const float progression = static_cast<float>(point) / static_cast<float>(deltaTime);
            if(ok) *ok = true;
            return _shot[i - 1].weight + progression * (_shot[i].weight - _shot[i - 1].weight);
        }
    }

    if(timeMs <= _shot[last].time) {
        if(ok) *ok = true;
    }
    return _shot[last].weight;
}

float shot_rate_between_indices(uint16_t firstIndex, uint16_t secondIndex) {
    const uint16_t last = shot_last_index();
    if(firstIndex == 0 || secondIndex == 0 || firstIndex >= secondIndex || secondIndex > last) return -1.0f;

    const int32_t weightDiff = _shot[secondIndex].weight - _shot[firstIndex].weight;
    const uint32_t timeDiff = _shot[secondIndex].time - _shot[firstIndex].time;
    if(weightDiff <= 0 || timeDiff == 0) return -1.0f;

    return static_cast<float>(weightDiff) / static_cast<float>(timeDiff);
}

float shot_rate_between_weights(int32_t startWeightMg, int32_t endWeightMg) {
    if(endWeightMg <= startWeightMg) return -1.0f;

    const uint16_t first = shot_index_at_or_after_weight(startWeightMg);
    const uint16_t second = shot_index_at_or_after_weight(endWeightMg);
    return shot_rate_between_indices(first, second);
}

float shot_rate_between_times(uint32_t startMs, uint32_t endMs) {
    if(endMs <= startMs + 200) return -1.0f;

    bool startOk = false;
    bool endOk = false;
    const int32_t startWeight = shot_weight_at_time(startMs, &startOk);
    const int32_t endWeight = shot_weight_at_time(endMs, &endOk);
    if(!startOk || !endOk || endWeight <= startWeight) return -1.0f;

    return static_cast<float>(endWeight - startWeight) / static_cast<float>(endMs - startMs);
}

void update_gbw_error_trend(int32_t overshootMg) {
    if(abs_mg(overshootMg) <= kGbwGoodShotBandMg) {
        gbwErrorTrendMg = (gbwErrorTrendMg * 3) / 4;
        if(gbwLearningConfidence > 0) gbwLearningConfidence--;
        return;
    }

    const bool sameDirection = gbwErrorTrendMg == 0 ||
                               (gbwErrorTrendMg > 0 && overshootMg > 0) ||
                               (gbwErrorTrendMg < 0 && overshootMg < 0);
    if(!sameDirection) {
        gbwErrorTrendMg = overshootMg;
        gbwLearningConfidence = 1;
        return;
    }

    gbwErrorTrendMg = (gbwErrorTrendMg * 3 + overshootMg) / 4;
    if(gbwLearningConfidence < kGbwMaxLearningConfidence) gbwLearningConfidence++;
}

bool apply_gbw_phase_learning(int32_t overshootMg, float slowRateMgPerMs) {
    update_gbw_error_trend(overshootMg);

    if(Menu3[GBW_SLOW_MG].value <= 0 || gbwLearningConfidence < kGbwSlowMgConfidence) {
        return false;
    }

    bool changed = false;
    const int32_t trend = gbwErrorTrendMg;
    const int32_t slowMgStep = abs_mg(trend) >= kGbwStrongAdjustBandMg
                                   ? Menu3[GBW_SLOW_MG].scalar * 2
                                   : Menu3[GBW_SLOW_MG].scalar;

    if(trend > kGbwAdjustBandMg) {
        const int16_t newSlowMg = clamp_enabled_slow_mg(Menu3[GBW_SLOW_MG].value + slowMgStep);
        if(newSlowMg != Menu3[GBW_SLOW_MG].value) {
            Menu3[GBW_SLOW_MG].value = newSlowMg;
            changed = true;
        }

        if(gbwLearningConfidence >= kGbwSlowRpmConfidence &&
           slowRateMgPerMs > 0.0f &&
           abs_mg(trend) >= kGbwStrongAdjustBandMg) {
            const int16_t newSlowRpm = clamp_slow_rpm(Menu3[GBW_SLOW_RPM].value - Menu3[GBW_SLOW_RPM].scalar);
            if(newSlowRpm != Menu3[GBW_SLOW_RPM].value) {
                Menu3[GBW_SLOW_RPM].value = newSlowRpm;
                changed = true;
            }
        }
    } else if(trend < -kGbwAdjustBandMg) {
        const int16_t newSlowMg = clamp_enabled_slow_mg(Menu3[GBW_SLOW_MG].value - slowMgStep);
        if(newSlowMg != Menu3[GBW_SLOW_MG].value) {
            Menu3[GBW_SLOW_MG].value = newSlowMg;
            changed = true;
        }

        if(gbwLearningConfidence >= kGbwSlowRpmConfidence &&
           slowRateMgPerMs > 0.0f &&
           abs_mg(trend) >= kGbwStrongAdjustBandMg) {
            const int16_t newSlowRpm = clamp_slow_rpm(Menu3[GBW_SLOW_RPM].value + Menu3[GBW_SLOW_RPM].scalar);
            if(newSlowRpm != Menu3[GBW_SLOW_RPM].value) {
                Menu3[GBW_SLOW_RPM].value = newSlowRpm;
                changed = true;
            }
        }
    }

    return changed;
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
    if(gbw_started == true && grindingComplete == false) ledAction(100 + 50.f*(float(currentWeight)/float(setWeight)));
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
            float stillToGrind = float(setWeight) - float(abs_mg(currentWeight));

            if(speed > 0.0f && speed < 20.0f) {
                predictedTime = (int32_t)(stillToGrind / speed);
            } else { 
                const int16_t fallbackRpm = slow_phase
                                                ? clamp_gbw_rpm(min(Menu3[GBW_SLOW_RPM].value, Menu3[GBW_RPM_SET].value))
                                                : clamp_gbw_rpm(Menu3[GBW_RPM_SET].value);
                predictedTime =  (int32_t)(stillToGrind / ((Menu3[GBW_SPEEDMOD].value/10000.f) * (fallbackRpm / 60.f)));
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
    const int32_t finalWeight = abs_mg(currentWeight);
    const int32_t targetWeight = static_cast<int32_t>(setWeight);
    const int32_t overshoot = finalWeight - targetWeight;
    bool isProper = true;
    bool stopEstimateOk = false;
    bool slowEstimateOk = false;
    int16_t newOffset = Menu3[GBW_OFFSET].value;

    const uint16_t stopIndex = shot_index_at_or_after_time(shotStopped);
    const int32_t stopWeight = shot_weight_at_time(shotStopped, &stopEstimateOk);
    const int32_t slowStartWeight = slow_phase_at > 0 ? shot_weight_at_time(slow_phase_at, &slowEstimateOk) : 0;
    const float stopSpeed = gbw_liveRateMgPerMs(4, stopIndex);

    int32_t bulkEndWeight = 10000;
    if(targetWeight > 5000 && bulkEndWeight > targetWeight - 2500) bulkEndWeight = targetWeight - 2500;
    if(slowEstimateOk && slowStartWeight > 3500 && bulkEndWeight > slowStartWeight - 500) {
        bulkEndWeight = slowStartWeight - 500;
    }
    if(bulkEndWeight < 3500) bulkEndWeight = 3500;

    const uint16_t firstVal = shot_index_at_or_after_weight(2000);
    const uint16_t secondVal = shot_index_at_or_after_weight(bulkEndWeight);
    const float grindSpeed = shot_rate_between_indices(firstVal, secondVal);
    const uint32_t test_time_diff = (firstVal != 0 && secondVal != 0 && secondVal > firstVal)
                                        ? _shot[secondVal].time - _shot[firstVal].time
                                        : 0;

    const uint32_t slowRateStart = slow_phase_at + 250;
    const uint32_t slowRateEnd = shotStopped > 150 ? shotStopped - 150 : shotStopped;
    const float slowRate = slow_phase_at > 0 ? shot_rate_between_times(slowRateStart, slowRateEnd) : -1.0f;

    const float rpm = clamp_gbw_rpm(Menu3[GBW_RPM_SET].value);
    if(rpm == 0 || Menu3[GBW_OFFSET].value <= 0) return;

    const float ground_per_rotation = grindSpeed / (rpm / 60.f);
    const float oldSpeedModifier = Menu3[GBW_SPEEDMOD].value / 10000.0f;
    speedModifier = oldSpeedModifier > 0.0f
                        ? (oldSpeedModifier * 3.0f + ground_per_rotation) / 4.0f
                        : ground_per_rotation;

    const int32_t actualDeltaWeight = finalWeight - stopWeight;
    const float endingSpeed = static_cast<float>(actualDeltaWeight) / static_cast<float>(Menu3[GBW_OFFSET].value);
    int32_t idealOffset = Menu3[GBW_OFFSET].value;
    if(endingSpeed > 0.0f) {
        idealOffset = static_cast<int32_t>(Menu3[GBW_OFFSET].value + (static_cast<float>(overshoot) / endingSpeed) / 4.0f);
    }

    if(overshoot > kGbwGoodShotBandMg && idealOffset > Menu3[GBW_OFFSET].value) {
        newOffset = clamp_menu_value(Menu3[GBW_OFFSET], (Menu3[GBW_OFFSET].value * 3 + idealOffset) / 4);
    }
    if(overshoot < -kGbwGoodShotBandMg && idealOffset < Menu3[GBW_OFFSET].value) {
        newOffset = clamp_menu_value(Menu3[GBW_OFFSET], (Menu3[GBW_OFFSET].value * 3 + idealOffset) / 4);
    }
    if(abs_mg(overshoot) <= kGbwGoodShotBandMg) newOffset = Menu3[GBW_OFFSET].value;

    if(!stopEstimateOk || stopWeight <= 0) isProper = false;
    if(actualDeltaWeight <= 0 || endingSpeed <= 0.0f) isProper = false;
    if(idealOffset < 50 || idealOffset > 500) isProper = false;
    if(abs_mg(overshoot) > 5000) isProper = false;
    if(test_time_diff < 600) isProper = false;
    if(firstVal == 0 || secondVal == 0 || secondVal <= firstVal) isProper = false;
    if(grindSpeed <= 0.0f || grindSpeed > 20.0f) isProper = false;
    if(speedModifier < 0.03f || speedModifier > 0.8f) isProper = false;

    #ifdef DEBUG_GBW
        Serial.print("last shot updated idx: "), Serial.println(last_shot_updated);
        Serial.print("final weight: "), Serial.println(finalWeight);
        Serial.print("target weight: "), Serial.println(setWeight);
        Serial.print("grinding duration: "), Serial.print(shotStopped), Serial.println("ms");
        Serial.print("grinding stopped estimation: "), Serial.print(stopWeight), Serial.println("mg");
        Serial.print("speed at stopping mg/ms: "), Serial.println(stopSpeed);
        Serial.print("bulk speed mg/ms: "), Serial.println(grindSpeed);
        Serial.print("slow speed mg/ms: "), Serial.println(slowRate);
        Serial.print("overshoot: "), Serial.println(overshoot);
        Serial.print("old speedmodifier: "), Serial.println(Menu3[GBW_SPEEDMOD].value);
        Serial.print("new speedmodifier: "), Serial.println(speedModifier * 10000);
        Serial.print("actual delta weight: "), Serial.println(actualDeltaWeight);
        Serial.print("endingSpeed: "), Serial.println(endingSpeed);
        Serial.print("idealOffset: "), Serial.println(idealOffset);
        Serial.print("last offset: "), Serial.println(Menu3[GBW_OFFSET].value);
    #endif

    if(isProper == true) {
        const bool phaseChanged = apply_gbw_phase_learning(overshoot, slowRate);
        (void)phaseChanged;
        #ifdef DEBUG_GBW
            Serial.println("Proper shot! saving values!");
            Serial.print("phase learning changed: "), Serial.println(phaseChanged);
            Serial.print("gbw trend mg: "), Serial.println(gbwErrorTrendMg);
            Serial.print("gbw confidence: "), Serial.println(gbwLearningConfidence);
        #endif
        Menu3[GBW_SPEEDMOD].value = clamp_menu_value(Menu3[GBW_SPEEDMOD], speedModifier * 10000);
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
