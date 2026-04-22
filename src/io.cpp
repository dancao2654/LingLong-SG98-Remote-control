#include "definitions.h"
#include "io.h"
#include "main.h"
#include <math.h>
#include <JC_Button.h>
#include <ESP32Encoder.h>
#include "pdata.h"
#include "display.h"
#include "gbw.h"
#include "comm.h"
#include "motor_hal.h"
#include "wifi_ui.h"
#include "radio_mode.h"

namespace {
constexpr uint32_t START_BUTTON_DEBOUNCE_MS = 12;
constexpr uint32_t ENC_BUTTON_DEBOUNCE_MS = 12;

bool encButtonIdleLevel = true;
bool encButtonLastRawLevel = true;
bool encButtonPressed = false;
bool encButtonChanged = false;
bool encButtonPressEvent = false;
bool encButtonLongPressLatched = false;
bool encButtonShortReleaseEvent = false;
bool encButtonLongPressEvent = false;
unsigned long encButtonDebounceStartedAt = 0;
unsigned long encButtonLastStableChangeAt = 0;

void pollEncoderButton() {
#if !SG98_HAS_ENCODER_BUTTON
    encButtonChanged = false;
    return;
#else
    const unsigned long now = millis();
    const bool rawLevel = digitalRead(ENC_BTN);

    encButtonChanged = false;
    if(rawLevel != encButtonLastRawLevel) {
        encButtonLastRawLevel = rawLevel;
        encButtonDebounceStartedAt = now;
    }

    if(now - encButtonDebounceStartedAt < ENC_BUTTON_DEBOUNCE_MS) {
        return;
    }

    const bool pressed = (rawLevel != encButtonIdleLevel);
    if(pressed != encButtonPressed) {
        encButtonPressed = pressed;
        encButtonChanged = true;
        encButtonLastStableChangeAt = now;
        if(pressed) {
            encButtonPressEvent = true;
            encButtonLongPressLatched = false;
        } else {
            encButtonPressEvent = false;
            if(!encButtonLongPressLatched) {
                encButtonShortReleaseEvent = true;
            }
        }
    }

    if(encButtonPressed && !encButtonLongPressLatched && now - encButtonLastStableChangeAt >= LONGPRESS) {
        encButtonLongPressLatched = true;
        encButtonLongPressEvent = true;
    }
#endif
}

bool shouldOpenDriveSelector() {
    return motor_get_drive_type() == DRIVE_TYPE_UNKNOWN || error == 1;
}

void openDriveSelector(States returnState) {
    motorSelectReturnState = returnState;

    switch(motor_get_drive_type()) {
        case DRIVE_TYPE_JMC:
            motorSelectSelected = MOTOR_SELECT_JMC;
            break;
        case DRIVE_TYPE_RT:
            motorSelectSelected = MOTOR_SELECT_RT;
            break;
        case DRIVE_TYPE_UNKNOWN:
        default:
            motorSelectSelected = MOTOR_SELECT_AUTO;
            break;
    }

    enterMenu = false;
    state = MOTOR_SELECT;
    disp_updateRequired = true;
}

void applyDriveSelection() {
    switch(motorSelectSelected) {
        case MOTOR_SELECT_JMC:
            motor_force_drive_type(DRIVE_TYPE_JMC, false);
            break;
        case MOTOR_SELECT_RT:
            motor_force_drive_type(DRIVE_TYPE_RT, false);
            break;
        case MOTOR_SELECT_AUTO:
        default:
            motor_redetect_drive(false);
            break;
    }

    error = 0;
    errorCode = 0;
    commCounter = 0;
    controller_note_activity();
    state = motorSelectReturnState;
}

bool isJmcBaudMenuItem(int selectedIdx) {
    return selectedIdx >= Menu4Items::JMCBAUD9600 && selectedIdx <= Menu4Items::JMCBAUD115200;
}

uint32_t baudForJmcMenuItem(int selectedIdx) {
    switch(selectedIdx) {
        case Menu4Items::JMCBAUD9600: return 9600;
        case Menu4Items::JMCBAUD19200: return 19200;
        case Menu4Items::JMCBAUD38400: return 38400;
        case Menu4Items::JMCBAUD57600: return 57600;
        case Menu4Items::JMCBAUD115200: return 115200;
        default: return 0;
    }
}
}
ESP32Encoder encoder;
Button start_button(BTN, START_BUTTON_DEBOUNCE_MS);

int16_t storedSetRPM = 0;
uint32_t storedSetWeight = 0;

void io_init() { 
#if SG98_HAS_START_BUTTON
    pinMode(BTN, INPUT_PULLUP);
    start_button.begin();
#endif

#if SG98_HAS_ENCODER_BUTTON
    pinMode(ENC_BTN, INPUT);
    encButtonIdleLevel = digitalRead(ENC_BTN);
    encButtonLastRawLevel = encButtonIdleLevel;
#else
    encButtonIdleLevel = true;
    encButtonLastRawLevel = true;
#endif
    encButtonPressed = false;
    encButtonChanged = false;
    encButtonPressEvent = false;
    encButtonLongPressLatched = false;
    encButtonShortReleaseEvent = false;
    encButtonLongPressEvent = false;
    encButtonDebounceStartedAt = millis();
    encButtonLastStableChangeAt = encButtonDebounceStartedAt;

#if SG98_HAS_START_LED
    pinMode(start_button_led, OUTPUT);
    ledcSetup(0,2000,10);
    ledcAttachPin(start_button_led, 0);
#endif

#if SG98_HAS_ENCODER
    pinMode(ENC_A, INPUT);
    pinMode(ENC_B, INPUT);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    encoder.attachHalfQuad(ENC_A, ENC_B);
    encoder.setFilter(100);
    encoder.setCount(0);
#endif
}

void do_io() { 
    static bool lastIdle = false; //lastIdle true means it was on normal Idle before, false means idle_GBW
    static int16_t OriginalValue = 0;
    static bool notReleasedYet = false;
    pollEncoderButton();

    if(state == IDLE) { 
        controller_idle_sleep_active() ? ledAction(0) : ledAction(3);
        setRPM += encoder_change()*rpm_scalar;

        if(setRPM > Menu1[SETMAX].value) setRPM = Menu1[SETMAX].value;
        if(setRPM < Menu1[SETMIN].value) setRPM = Menu1[SETMIN].value;
        
        //START BUTTON
        if(START_BUTTON() == true && dimmed == false) {
            if(shouldOpenDriveSelector()) openDriveSelector(IDLE);
            else {
                purge_prepare_for_grinding();
                state = GRINDING;
            }
        }

        if(pressedLong() == true && dimmed == false) {
            state = IDLE_GBW;
            disp_updateRequired = true;
            pdata_write(5);
        } else if(ENC_BUTTON() == true && dimmed == false) {
            if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
                openDriveSelector(IDLE);
            } else {
                if(error == 0 || error == 1 || error == 2) state = MENU1;
                lastIdle = true;
                if(error > 2) { 
                    error = 0;
                    commCounter = 0;
                }
                disp_updateRequired = true;
            }
        }

        if(setRPM != storedSetRPM && lastActivity + 10000 < millis()) {
            storedSetRPM = setRPM;
            pdata_write(3);
        }

    }

    if(state == IDLE_GBW) { 
        controller_idle_sleep_active() ? ledAction(0) : ledAction(3);
        setWeight += encoder_change() * 100;
        
        if(setWeight > default_max_weight) setWeight = default_max_weight;
        if(setWeight < default_min_weight) setWeight = default_min_weight;
        
        if(START_BUTTON() == true) {
            if(shouldOpenDriveSelector()) openDriveSelector(IDLE_GBW);
            else state = GRINDING_GBW;
        }

        if(pressedLong() == true && dimmed == false) {
            state = IDLE;
            disp_updateRequired = true;
            pdata_write(5);
        } else if(ENC_BUTTON() == true && dimmed == false) {
            if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
                openDriveSelector(IDLE_GBW);
            } else {
                if(error == 0 || error == 1 || error == 2) state = MENU1;
                lastIdle = false;
                if(error > 2) { 
                    error = 0;
                    commCounter = 0;
                }
                disp_updateRequired = true;
            }
        }
        
        if(setWeight != storedSetWeight && lastActivity + 10000 < millis()) {
            storedSetWeight = setWeight;
            pdata_write(3);
        }

    }

    if(state == GRINDING) { 
        //Flash responsively based on auto purge.
        if(ready_purge == false) ledAction(2);
        else ledAction(4);

        setRPM += encoder_change()*rpm_scalar;

        if(setRPM > Menu1[SETMAX].value) setRPM = Menu1[SETMAX].value;
        if(setRPM < Menu1[SETMIN].value) setRPM = Menu1[SETMIN].value;

        if(START_BUTTON() == true) { 
            purge_mark_started();
            state = (Menu2[SETBUTTONPURGE].value == true? PURGING : IDLE);
            if(state == IDLE) purge_cancel_pending();
        }
        if(ENC_BUTTON_DOWN() == true) { 
            purge_cancel_pending();
            state = IDLE;
            motor_setRPM = 0;
        }
    }

    if(state == PURGING) { 
        ledAction(1);
        setRPM += encoder_change()*rpm_scalar;

        if(START_BUTTON() == true || ENC_BUTTON_DOWN() == true) { 
            purge_cancel_pending();
            state = IDLE;
            motor_setRPM = 0;
        }
    }

    if(state == GRINDING_GBW) { 
        //setWeight += encoder_change();

        if(START_BUTTON() == true || ENC_BUTTON_DOWN() == true) { 
            state = IDLE_GBW;
            motor_setRPM = 0;
        }

    }
    if(state == MENU1) { 
        //Encoder routes
        if(enterMenu == false) { 
            menu1Selected += Menu1[INVERT_SCROLL].value? encoder_change() : -encoder_change();
            ledAction(0);
        } 
        if(menu1Selected > NUM_MENU1_ITEMS-1) menu1Selected = 0;
        if(menu1Selected < 0) menu1Selected = NUM_MENU1_ITEMS - 1;

        if(enterMenu == true) { 
            Menu1[menu1Selected].value += encoder_change() * Menu1[menu1Selected].scalar;
            if(Menu1[menu1Selected].value > Menu1[menu1Selected].maxValue) Menu1[menu1Selected].value = Menu1[menu1Selected].maxValue;
            if(Menu1[menu1Selected].value < Menu1[menu1Selected].minValue) Menu1[menu1Selected].value = Menu1[menu1Selected].minValue;
            if(menu1Selected == Menu1Items::SETMAX && Menu1[SETMAX].value < Menu1[SETMIN].value) {
                Menu1[SETMAX].value = Menu1[SETMIN].value;
            }
            if(menu1Selected == Menu1Items::SETMIN && Menu1[SETMIN].value > Menu1[SETMAX].value) {
                Menu1[SETMIN].value = Menu1[SETMAX].value;
            }
            if(menu1Selected == Menu1Items::CALIBRATE) { 
                //DISPLAY press start to continue
                ledAction(2);
            } else if (menu1Selected == Menu1Items::LED_BRIGHTNESS) { ledAction(100+Menu1[menu1Selected].value); } 
            else { ledAction(0); } 
        }

        //Button routes
        if(START_BUTTON() == true) {
            if (enterMenu == false) { 
                menu1Selected = EXITMENU;
                lastIdle? state = IDLE : state = IDLE_GBW; 
            } else { 
                enterMenu = false;
                Menu1[menu1Selected].value = OriginalValue;
                menu1Selected = EXITMENU;
            }
        }

        if(ENC_BUTTON_DOWN() == true) { 
            if(enterMenu == false) { 
                if(menu1Selected == Menu1Items::PURGESETTINGS) state = MENU2;
                if(menu1Selected == Menu1Items::GBWSETTINGS) state = MENU3;
                if(menu1Selected == Menu1Items::JMCBAUDMENU) state = MENU4;
                if(menu1Selected == Menu1Items::EXITMENU) lastIdle? state = IDLE : state = IDLE_GBW; 
                if(menu1Selected != Menu1Items::PURGESETTINGS && menu1Selected != Menu1Items::GBWSETTINGS && menu1Selected != Menu1Items::JMCBAUDMENU && menu1Selected != Menu1Items::EXITMENU) enterMenu = true; //entering settings adjustment
                OriginalValue = Menu1[menu1Selected].value;
            } else { 
                enterMenu = false;
                if(menu1Selected == Menu1Items::RADIOMODE) {
                    if(radio_mode_apply()) {
                        pdata_write(2);
                    } else {
                        Menu1[menu1Selected].value = OriginalValue;
                        disp_updateRequired = true;
                    }
                } else if(menu1Selected != Menu1Items::RESET && menu1Selected != Menu1Items::RESETNETWORK) {
                    pdata_write(2);
                }
                pdata_normalize();
                if(menu1Selected == Menu1Items::RESETNETWORK && Menu1[menu1Selected].value == 1) {
                    wifi_ui_reset_network_settings();
                    Menu1[menu1Selected].value = 0;
                    disp_updateRequired = true;
                }
                if(menu1Selected == Menu1Items::RESET && Menu1[menu1Selected].value == 1) { 
                    pdata_write(8);  // Reset the build ID so it resets on boot.
                    motorReset();
                    delay(1000);
                    if(motor_get_drive_type() == DRIVE_TYPE_JMC) sleep_init();
                    else ESP.restart();
                } else if(menu1Selected == Menu1Items::RESETNETWORK) {
                    Menu1[menu1Selected].value = 0;
                }
            }
        } else if(pressedLong() == true && menu1Selected == Menu1Items::CALIBRATE) {
            state = CALIBRATING;  
            enterMenu = false;
            disp_updateRequired = true;
            notReleasedYet = true;
        }
        
    }

    if(state == MENU2) { 
        //ENCODER ROUTE
        if(enterMenu == false) { 
            menu2Selected += Menu1[INVERT_SCROLL].value? encoder_change() : -encoder_change();
            ledAction(0);
        }

        if(menu2Selected > Menu2Items::NUM_MENU2_ITEMS - 1) menu2Selected = 0;
        if(menu2Selected < 0) menu2Selected = Menu2Items::NUM_MENU2_ITEMS - 1;

        if(enterMenu == true) { 
            Menu2[menu2Selected].value += encoder_change() * Menu2[menu2Selected].scalar;
            if(Menu2[menu2Selected].value > Menu2[menu2Selected].maxValue) Menu2[menu2Selected].value = Menu2[menu2Selected].maxValue;
            if(Menu2[menu2Selected].value < Menu2[menu2Selected].minValue) Menu2[menu2Selected].value = Menu2[menu2Selected].minValue;
            
            if(menu2Selected == Menu2Items::SETBUTTONPURGE && Menu2[SETBUTTONPURGE].value == true) { 
                ledAction(2);
            } else ledAction(0);
        }

        //BUTTON ROUTES
        if(START_BUTTON() == true) { 
            if(enterMenu == false) { 
                menu2Selected = Menu2Items::RETURN_FROM_PURGE;
                state = MENU1;
            } else { 
                enterMenu = false;
                Menu2[menu2Selected].value = OriginalValue;
            }
        } 

        if(ENC_BUTTON_DOWN() == true) { 
            if(enterMenu == false) { 
                if(menu2Selected == Menu2Items::RETURN_FROM_PURGE) state = MENU1;
                if(menu2Selected != Menu2Items::RETURN_FROM_PURGE) enterMenu = true;
                OriginalValue = Menu2[menu2Selected].value;
            } else { 
                enterMenu = false;
                pdata_write(2);
                pdata_normalize();
            }
        }
    }

    if(state == MENU3) { 
        ledAction(0);
        //Encoder routes
        if(enterMenu == false) menu3Selected += Menu1[INVERT_SCROLL].value? encoder_change() : -encoder_change();
        if(menu3Selected > Menu3Items::NUM_MENU3_ITEMS - 1) menu3Selected = 0;
        if(menu3Selected < 0) menu3Selected = Menu3Items::NUM_MENU3_ITEMS - 1;

        if(enterMenu == true) { 
            Menu3[menu3Selected].value += encoder_change()* Menu3[menu3Selected].scalar;
            if(Menu3[menu3Selected].value > Menu3[menu3Selected].maxValue) Menu3[menu3Selected].value = Menu3[menu3Selected].maxValue;
            if(Menu3[menu3Selected].value < Menu3[menu3Selected].minValue) Menu3[menu3Selected].value = Menu3[menu3Selected].minValue;
            if(menu3Selected == Menu3Items::GBW_RPM_SET) {
                int16_t slowLimit = Menu3[GBW_SLOW_RPM].value;
                if(slowLimit > Menu1[SETMAX].value) slowLimit = Menu1[SETMAX].value;
                if(Menu3[GBW_RPM_SET].value > Menu1[SETMAX].value) Menu3[GBW_RPM_SET].value = Menu1[SETMAX].value;
                if(Menu3[GBW_RPM_SET].value < slowLimit) Menu3[GBW_RPM_SET].value = slowLimit;
            }
            if(menu3Selected == Menu3Items::GBW_SLOW_RPM) {
                if(Menu3[GBW_SLOW_RPM].value > Menu1[SETMAX].value) Menu3[GBW_SLOW_RPM].value = Menu1[SETMAX].value;
                if(Menu3[GBW_SLOW_RPM].value > Menu3[GBW_RPM_SET].value) Menu3[GBW_SLOW_RPM].value = Menu3[GBW_RPM_SET].value;
            }
        }

        //Button routes
        if(START_BUTTON() == true) { 
            if(enterMenu == false) { 
                menu3Selected = Menu3Items::RETURN_FROM_GBW;
                state = MENU1;
            } else { 
                enterMenu = false;
                Menu3[menu3Selected].value = OriginalValue;
            }
        } 

        if(ENC_BUTTON_DOWN() == true) { 
            if(enterMenu == false) { 
                if(menu3Selected == RETURN_FROM_GBW) state = MENU1;
                if(menu3Selected != RETURN_FROM_GBW) enterMenu = true;
                OriginalValue = Menu3[menu3Selected].value;
            } else { 
                enterMenu = false;
                // Special case
                if(menu3Selected == 7) {
                    if(Menu3[SAVE_SCALE].value == 0) {
                        pdata_write(6);  //remove stored scale stuff
                        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
                            scale_connect_mac = "";
                            scale_connect_name = "";
                            xSemaphoreGive(scaleMutex);
                        } 
                    }
                    if(Menu3[SAVE_SCALE].value == 1) { 
                        if (xSemaphoreTake(scaleMutex, portMAX_DELAY)) {
                            if(scale_mac != "") {
                                scale_connect_mac = scale_mac;
                                scale_connect_name = scale_name;
                            }
                            xSemaphoreGive(scaleMutex);
                        } 
                        pdata_write(7);
                    }
                } else 
                pdata_write(2);
                pdata_normalize();

            
            }
        } 
    }

    if(state == MOTOR_SELECT) {
        ledAction(0);
        motorSelectSelected += Menu1[INVERT_SCROLL].value ? encoder_change() : -encoder_change();
        if(motorSelectSelected >= NUM_MOTOR_SELECT_ITEMS) motorSelectSelected = 0;
        if(motorSelectSelected < 0) motorSelectSelected = NUM_MOTOR_SELECT_ITEMS - 1;

        if(START_BUTTON() == true) {
            state = motorSelectReturnState;
            disp_updateRequired = true;
        }

        if(ENC_BUTTON_DOWN() == true) {
            applyDriveSelection();
        }
    }

    if(state == MENU4) {
        ledAction(0);
        if(enterMenu == false) menu4Selected += Menu1[INVERT_SCROLL].value ? encoder_change() : -encoder_change();
        if(menu4Selected > Menu4Items::NUM_MENU4_ITEMS - 1) menu4Selected = 0;
        if(menu4Selected < 0) menu4Selected = Menu4Items::NUM_MENU4_ITEMS - 1;

        if(START_BUTTON() == true) {
            if(enterMenu == false) {
                menu4Selected = Menu4Items::RETURN_FROM_JMC_BAUD;
                state = MENU1;
            } else {
                enterMenu = false;
            }
        }

        if(ENC_BUTTON_DOWN() == true) {
            if(enterMenu == false) {
                if(menu4Selected == Menu4Items::RETURN_FROM_JMC_BAUD) {
                    state = MENU1;
                } else {
                    enterMenu = true;
                }
            } else {
                motor_run_jmc_baud_switch(baudForJmcMenuItem(menu4Selected));
                disp_updateRequired = true;
                update_display();
            }
        }
    }

    if(state == CALIBRATING) { 
        if(ENC_BUTTON() == true) { 
            if(notReleasedYet == true) notReleasedYet = false;
            else {
                motorOff();
                error = 102;
                state = lastIdle? IDLE : IDLE_GBW;
                enterMenu = false;
            }
        }
        if(START_BUTTON() == true) {
            motorOff();
            error = 102;
            state = lastIdle? IDLE : IDLE_GBW;
            enterMenu = false;
        }

        ledAction(1);
    }

}

bool pressedLong() { 
    if(encButtonLongPressEvent) {
        encButtonLongPressEvent = false;
        encButtonPressEvent = false;
        controller_note_activity();
        return true;
    }

    return false;
}

int16_t encoder_change() { 
#if !SG98_HAS_ENCODER
    return 0;
#else
    static int32_t lastV = 0;
    int32_t v = 0;
    int16_t change = 0;

    v = encoder.getCount()/ENC_TOL; 
    change = lastV - v;
    lastV = v;

    if(change != 0) { 
        controller_note_activity();
    }
    return change;
#endif
}

bool check_io() { //returns true immediately when a button is pressed - normal control uses wasReleased to prevent double feedback.
    pollEncoderButton();
#if SG98_HAS_START_BUTTON
    start_button.read();
    if(encButtonPressed || encButtonPressEvent || encButtonShortReleaseEvent || encButtonLongPressEvent) return true;
    if(start_button.isPressed() || start_button.wasPressed() || start_button.wasReleased()) return true;
#else
    if(encButtonPressed || encButtonPressEvent || encButtonShortReleaseEvent || encButtonLongPressEvent) return true;
#endif
    return false;
}

bool ENC_BUTTON_DOWN() {
    if(encButtonPressEvent) {
        encButtonPressEvent = false;
        encButtonLongPressLatched = true;
        controller_note_activity();
        return true;
    }
    return false;
}

bool ENC_BUTTON() { 
    if(encButtonShortReleaseEvent) {
        encButtonShortReleaseEvent = false;
        encButtonPressEvent = false;
        controller_note_activity();
        return true;
    }
    return false;
}

bool START_BUTTON() {
#if !SG98_HAS_START_BUTTON
    return false;
#else
    start_button.read();
    if (start_button.wasPressed()) {
        controller_note_activity();
        return true;
    }
    return false;
#endif
}

void ledAction(uint8_t type) {  //Type: 0 = off, 1 = on, 2 = flashing, 3 = fading, 4 flashing fast, 5 = flashing fastest, 100-200 = %.
#if !SG98_HAS_START_LED
    (void)type;
    return;
#else
    static unsigned long lastCall;
    static uint8_t lastType;
    static unsigned long lastFlash;
    static uint16_t lastLedValue;
    uint16_t ledValue;
    bool ledOn = true;
    
    // if(lastCall + 10 > millis()) return; //does this really add anything..? prob.

    if (type == 0) { 
        ledValue = 0;
    }

    if (type == 1) {
        ledValue = 1023;
    }

    //flashing
    if (type == 2 || type == 4 || type == 5) { 

        //On if starting from flash
        if(lastType != type) { 
            ledOn = true;
            ledValue = 1023;
            lastFlash = millis();
        }
        //flash
        if(type == 2) {
            if(lastFlash + flashTime < millis()) { 
                ledOn = false;
            }

            if(lastFlash + 2*flashTime < millis()) { 
                ledOn = true;
                lastFlash = millis();
            }
        }
        //faster
        if(type == 4) { 
            if(lastFlash + fastFlashTime < millis()) { 
                ledOn = false;
            }

            if(lastFlash + 2*fastFlashTime < millis()) { 
                ledOn = true;
                lastFlash = millis();
            }
        }
        //FASTER!!
        if(type == 5) { 
            if(lastFlash + fastestFlashTime < millis()) { 
                ledOn = false;
            }

            if(lastFlash + 2*fastestFlashTime < millis()) { 
                ledOn = true;
                lastFlash = millis();
            }            
        }

        if(ledOn == true) ledValue = 1023;
        else ledValue = 0;
    }

    if (type == 3) { //fading
        static float cosValue = 0;

        //Start from last value
        if (lastType != type) { 
            lastType == 1? cosValue = 2*PI : cosValue = PI;
            lastFlash = millis();
        }

        //calculate new value
        if(cosValue > 3*PI) cosValue = PI;
        cosValue += ((millis() - lastFlash)/(float)fadeTime) * PI;
        ledValue = ((cos(cosValue) + 1) * 1023) / 2;

        lastFlash = millis();
    }

    // Type between 100 and 200 -> direct value
    if (type >= 100 && type <= 200) { 
        ledValue = (type - 100)*10;
    }

    if(lastLedValue != ledValue) { 

        ledcWrite(0, ledValue*(float(Menu1[LED_BRIGHTNESS].value)/100));
        lastLedValue = ledValue;
    }

    lastType = type;
#endif
}
