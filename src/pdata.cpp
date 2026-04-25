#include <nvs_flash.h>
#include "pdata.h"
#include "gbw.h"
#include "main.h"
#include "gbw.h"
#include "io.h"
#include "motor_hal.h"
#include "radio_mode.h"

Preferences preferences; 

namespace {
constexpr uint32_t DEFAULT_JMC_BAUD = 9600;

bool clamp_menu_entry(menuEntry& entry) {
    int16_t original = entry.value;
    if(entry.value < entry.minValue) entry.value = entry.minValue;
    if(entry.value > entry.maxValue) entry.value = entry.maxValue;
    return entry.value != original;
}

uint32_t sanitize_jmc_baud(uint32_t baud_rate) {
    switch(baud_rate) {
        case 57600:
        case 38400:
        case 19200:
        case 115200:
        case 9600:
            return baud_rate;
        default:
            return DEFAULT_JMC_BAUD;
    }
}
}

void pdata_init() { 
    preferences.begin("settings", false);

    // Get stored firmware build ID
    String storedBuildID = preferences.getString("build_id", "");
    String currentBuildID = BUILD_ID;

    if (storedBuildID != currentBuildID) {
        // First boot OR firmware changed
        firstBootFlag = true;
        preferences.end();
        delay(200);
        nvs_flash_erase();   // Erase the entire flash partition used for NVS
        nvs_flash_init();    // Re-initialize NVS
        #ifdef DEBUG 
            Serial.println("First boot detected! Saving ALL");
        #endif
        delay(200);
        preferences.begin("settings", false);
        // Update stored build ID
    } else firstBootFlag = false;
}

void pdata_read() {  //reads ALL stored settings in flash
    int8_t readState;
    storedSetRPM = preferences.getShort("setRPM", default_setRPM);
    storedSetWeight = preferences.getULong("setWeight", default_setWeight);
    readState = preferences.getShort("idleState", 1); 

    if(readState == SLEEPING) state = IDLE;
    if(readState == IDLE) state = IDLE;
    if(readState == IDLE_GBW) state = IDLE_GBW;

    //Menu 1
    for(int i = 0; i < NUM_MENU1_ITEMS; i++) { 
        Menu1[i].value = preferences.getShort(Menu1[i].prefUID, Menu1[i].value);
    }
    //Menu2
    for(int i = 0; i < NUM_MENU2_ITEMS; i++) { 
        Menu2[i].value = preferences.getShort(Menu2[i].prefUID, Menu2[i].value);
    }
    //Menu 3
    for(int i = 0; i < NUM_MENU3_ITEMS; i++) { 
       Menu3[i].value = preferences.getShort(Menu3[i].prefUID, Menu3[i].value);
    }
    preferences.getBytes("calibrateArray", calibrateArray, SIZEOFCALIBRATEARRAY); //raw data, works?

    #ifdef DEBUG_CALIB
    for(int i = 0; i < (SIZEOFCALIBRATEARRAY/2); i++) { 
        Serial.print("Array"), Serial.print(i), Serial.print(" value: "), Serial.println(calibrateArray[i]);
    }
    #endif
    // //Retrieve stored scale info. 
    String scale_name = "", scale_mac = "";
    scale_connect_name = preferences.getString("SCALE_NAME", "");
    scale_connect_mac = preferences.getString("SCALE_MAC", "");

    //setrpm, weight
    setRPM = storedSetRPM;
    setWeight = storedSetWeight;
}

void pdata_normalize(bool persist) {
    bool menu1Changed[NUM_MENU1_ITEMS] = {false};
    bool menu2Changed[NUM_MENU2_ITEMS] = {false};
    bool menu3Changed[NUM_MENU3_ITEMS] = {false};
    bool setRPMChanged = false;
    bool setWeightChanged = false;

    for(int i = 0; i < NUM_MENU1_ITEMS; i++) {
        menu1Changed[i] = clamp_menu_entry(Menu1[i]);
    }
    for(int i = 0; i < NUM_MENU2_ITEMS; i++) {
        menu2Changed[i] = clamp_menu_entry(Menu2[i]);
    }
    for(int i = 0; i < NUM_MENU3_ITEMS; i++) {
        menu3Changed[i] = clamp_menu_entry(Menu3[i]);
    }

    if(Menu1[SETMIN].value > Menu1[SETMAX].value) {
        Menu1[SETMIN].value = Menu1[SETMAX].value;
        menu1Changed[SETMIN] = true;
    }

    const int16_t sanitizedRadioMode = static_cast<int16_t>(radio_mode_sanitize_value(Menu1[RADIOMODE].value));
    if(Menu1[RADIOMODE].value != sanitizedRadioMode) {
        Menu1[RADIOMODE].value = sanitizedRadioMode;
        menu1Changed[RADIOMODE] = true;
    }

    if(Menu3[GBW_SLOW_RPM].value > Menu3[GBW_RPM_SET].value) {
        Menu3[GBW_SLOW_RPM].value = Menu3[GBW_RPM_SET].value;
        menu3Changed[GBW_SLOW_RPM] = true;
    }

    if(setRPM > Menu1[SETMAX].value) {
        setRPM = Menu1[SETMAX].value;
        setRPMChanged = true;
    }
    if(setRPM < Menu1[SETMIN].value) {
        setRPM = Menu1[SETMIN].value;
        setRPMChanged = true;
    }

    if(setWeight > default_max_weight) {
        setWeight = default_max_weight;
        setWeightChanged = true;
    }
    if(setWeight < default_min_weight) {
        setWeight = default_min_weight;
        setWeightChanged = true;
    }

    if(!persist) return;

    for(int i = 0; i < NUM_MENU1_ITEMS; i++) {
        if(menu1Changed[i]) preferences.putShort(Menu1[i].prefUID, Menu1[i].value);
    }
    for(int i = 0; i < NUM_MENU2_ITEMS; i++) {
        if(menu2Changed[i]) preferences.putShort(Menu2[i].prefUID, Menu2[i].value);
    }
    for(int i = 0; i < NUM_MENU3_ITEMS; i++) {
        if(menu3Changed[i]) preferences.putShort(Menu3[i].prefUID, Menu3[i].value);
    }
    if(setRPMChanged) preferences.putShort("setRPM", setRPM);
    if(setWeightChanged) preferences.putULong("setWeight", setWeight);
}


void pdata_write(uint8_t what) { // 0 = ALL SETTIGNS , 1 = Calibration array, 2 = specific setting (derived from state && menuXselected), 3 = setWeight or RPM (based on state), 4 = GbW speedModifier, 5 = lastState; 6=remove last scale, 7 = save scale, 8 = write reset on build id
    if(what == 0) { //write ALL (default) settings *only used when first initalizing*
        //Menu 1
        for(int i = 0; i < NUM_MENU1_ITEMS; i++) { 
            preferences.putShort(Menu1[i].prefUID, Menu1[i].value);
        }
        //Menu2
        for(int i = 0; i < NUM_MENU2_ITEMS; i++) { 
            preferences.putShort(Menu2[i].prefUID, Menu2[i].value);
        }
        //Menu 3
        for(int i = 0; i < NUM_MENU3_ITEMS; i++) { 
            preferences.putShort(Menu3[i].prefUID, Menu3[i].value);
        }

        preferences.putShort("setRPM", default_setRPM);
        preferences.putULong("setWeight", default_setWeight);
        preferences.putFloat("speedModifier", speedModifier);
        preferences.putULong("jmcBaud", DEFAULT_JMC_BAUD);

        preferences.putString("SCALE_NAME", "");
        preferences.putString("SCALE_MAC", "");

        String currentBuildID = BUILD_ID;
        preferences.putString("build_id", currentBuildID);
        Serial.println("Stored ALL");

        //set array to 11 
        for(int i = 0; i < SIZEOFCALIBRATEARRAY; i++) { 
            calibrateArray[i] = 11;
        }
        preferences.putBytes("calibrateArray", calibrateArray, SIZEOFCALIBRATEARRAY); //This puts raw data in
    }
    if(what == 1) { //write calibrateArray
        preferences.remove("calibrateArray");
        preferences.putBytes("calibrateArray", calibrateArray, SIZEOFCALIBRATEARRAY); //This puts raw data in 
        #ifdef DEBUG_CALIB
        Serial.println("Stored 1");
        #endif
    }

    if(what == 2) { 
        if(state == MENU1) preferences.putShort(Menu1[menu1Selected].prefUID, Menu1[menu1Selected].value);
        if(state == MENU2) preferences.putShort(Menu2[menu2Selected].prefUID, Menu2[menu2Selected].value);
        if(state == MENU3) preferences.putShort(Menu3[menu3Selected].prefUID, Menu3[menu3Selected].value);
        #ifdef DEBUG
            Serial.println("Stored 2");
        #endif
    }

    //update set RPM or weight
    if(what == 3) {
        if(state == IDLE) preferences.putShort("setRPM", setRPM);
        if(state == IDLE_GBW) preferences.putULong("setWeight", setWeight);
        #ifdef DEBUG
            Serial.println("Stored 3");
        #endif
    }

    //update from gbw learn
    if(what == 4) { 
        if(state == GRINDING_GBW) preferences.putShort(Menu3[GBW_SPEEDMOD].prefUID, Menu3[GBW_SPEEDMOD].value);
        if(state == GRINDING_GBW) preferences.putShort(Menu3[GBW_OFFSET].prefUID, Menu3[GBW_OFFSET].value);  
        #ifdef DEBUG
            Serial.println("Stored 4");
        #endif
    }

    //update idle state stored
    if(what == 5) { 
        if(state == IDLE) preferences.putShort("idleState", 1);
        if(state == IDLE_GBW) preferences.putShort("idleState", 2);
       // Serial.println("Stored 5");
    }

    // Remove scale info
    if(what == 6) { 
        if(state != MENU3) return;
            preferences.putString("SCALE_NAME", "");
            preferences.putString("SCALE_MAC", "");
    }

    // save scale info
    if(what == 7) { 
        String macCopy;
        String nameCopy;
        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
            macCopy = scale_connect_mac;
            nameCopy = scale_connect_name;
            xSemaphoreGive(scaleMutex);
        } 
        preferences.putString("SCALE_NAME", nameCopy);
        preferences.putString("SCALE_MAC", macCopy);
    }

    if(what == 8 ) { 
        preferences.putString("build_id", "RESET"); // change build ID so it rewrites all settings on next boot
    }
}

uint32_t pdata_read_jmc_baud() {
    return sanitize_jmc_baud(preferences.getULong("jmcBaud", DEFAULT_JMC_BAUD));
}

void pdata_write_jmc_baud(uint32_t baud_rate) {
    preferences.putULong("jmcBaud", sanitize_jmc_baud(baud_rate));
}
