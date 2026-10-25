#include "definitions.h"
#include "burr_gap.h"
#include "controller_board.h"
#include "controller_ota.h"
#include "io.h"
#include "main.h"
#include <math.h>
#include <ESP32Encoder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "pdata.h"
#include "display.h"
#include "gbw.h"
#include "comm.h"
#include "motor_hal.h"
#include "wifi_ui.h"
#include "radio_mode.h"
#include "sg98_version.h"

namespace {
constexpr uint32_t START_BUTTON_DEBOUNCE_MS = 12;
constexpr uint32_t ENC_BUTTON_DEBOUNCE_MS = 12;
constexpr uint32_t BUTTON_SERVICE_POLL_MS = 2;
constexpr uint32_t BUTTON_SERVICE_STACK_WORDS = 2048;
constexpr UBaseType_t BUTTON_SERVICE_PRIORITY = 4;

portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t buttonTaskHandle = nullptr;

bool startButtonAvailable = false;
bool startButtonIdleLevel = true;
bool startButtonLastRawLevel = true;
bool startButtonPressed = false;
bool startButtonPressEvent = false;
bool startButtonReleaseEvent = false;
unsigned long startButtonDebounceStartedAt = 0;

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

bool showStartBlockedIfNeeded();

void showMenuEntryFeedback(const menuEntry& entry, const char* suffix = " applied", unsigned long durationMs = 900) {
    const char* name = entry.name != nullptr && entry.name[0] != '\0' ? entry.name : "Setting";
    const char* tail = suffix != nullptr ? suffix : "";
    char label[64];
    snprintf(label, sizeof(label), "%s%s", name, tail);
    display_show_action_feedback(label, durationMs);
}

void startCalibrationFromMenu(bool& notReleasedYet) {
    if(showStartBlockedIfNeeded()) {
        return;
    }

    error = 0;
    state = CALIBRATING;
    enterMenu = false;
    display_show_action_feedback("Calibration started");
    disp_updateRequired = true;
    notReleasedYet = true;
}

void pollEncoderButton() {
    const ControllerBoardPins& pins = controller_board_pins();
    if(!controller_board_has_encoder_button() || !controller_board_pin_valid(pins.encoderButton)) {
        portENTER_CRITICAL(&buttonMux);
        encButtonChanged = false;
        portEXIT_CRITICAL(&buttonMux);
        return;
    }

    const unsigned long now = millis();
    const bool rawLevel = digitalRead(pins.encoderButton);

    portENTER_CRITICAL(&buttonMux);
    encButtonChanged = false;
    if(rawLevel != encButtonLastRawLevel) {
        encButtonLastRawLevel = rawLevel;
        encButtonDebounceStartedAt = now;
    }

    if(now - encButtonDebounceStartedAt < ENC_BUTTON_DEBOUNCE_MS) {
        portEXIT_CRITICAL(&buttonMux);
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
    portEXIT_CRITICAL(&buttonMux);
}

void pollStartButton() {
    if(!startButtonAvailable) {
        return;
    }

    const ControllerBoardPins& pins = controller_board_pins();
    const unsigned long now = millis();
    const bool rawLevel = digitalRead(pins.startButton);

    portENTER_CRITICAL(&buttonMux);
    if(rawLevel != startButtonLastRawLevel) {
        startButtonLastRawLevel = rawLevel;
        startButtonDebounceStartedAt = now;
    }

    if(now - startButtonDebounceStartedAt < START_BUTTON_DEBOUNCE_MS) {
        portEXIT_CRITICAL(&buttonMux);
        return;
    }

    const bool pressed = (rawLevel != startButtonIdleLevel);
    if(pressed != startButtonPressed) {
        startButtonPressed = pressed;
        if(pressed) {
            startButtonPressEvent = true;
        } else {
            startButtonReleaseEvent = true;
        }
    }
    portEXIT_CRITICAL(&buttonMux);
}

void buttonServiceTask(void*) {
    for(;;) {
        pollEncoderButton();
        pollStartButton();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SERVICE_POLL_MS));
    }
}

void startButtonServiceTask() {
    if(buttonTaskHandle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore(buttonServiceTask,
                            "ButtonTask",
                            BUTTON_SERVICE_STACK_WORDS,
                            nullptr,
                            BUTTON_SERVICE_PRIORITY,
                            &buttonTaskHandle,
                            1);
}

bool shouldOpenDriveSelector() {
    return motor_get_drive_type() == DRIVE_TYPE_UNKNOWN || error == 1;
}

bool controllerStartAllowed(const char** message) {
    if(error > 2 && error < 100) {
        *message = "Clear motor error";
        return false;
    }
    if(commStatus != COMM_CONNECTED || currentStatus == MOTOR_NOT_CONNECTED) {
        *message = "Motor offline";
        return false;
    }
    if(currentStatus == MOTOR_FAULT) {
        *message = "Motor fault";
        return false;
    }
    if(currentStatus == MOTOR_INVALID) {
        *message = "Motor status invalid";
        return false;
    }

    *message = nullptr;
    return true;
}

bool showStartBlockedIfNeeded() {
    const char* message = nullptr;
    if(controllerStartAllowed(&message)) {
        return false;
    }

    motor_setRPM = 0;
    display_show_action_feedback(message != nullptr ? message : "Start blocked", 1000);
    disp_updateRequired = true;
    return true;
}

uint8_t menuItemCount(uint8_t menuNum) {
    switch(menuNum) {
        case 1: return NUM_MENU1_ITEMS;
        case 2: return NUM_MENU2_ITEMS;
        case 3: return NUM_MENU3_ITEMS;
        case 4: return NUM_MENU4_ITEMS;
        case 5: return NUM_MENU5_ITEMS;
        default: return 0;
    }
}

void ensureVisibleMenuSelection(uint8_t menuNum, int8_t& selected) {
    const uint8_t count = menuItemCount(menuNum);
    if(count == 0) {
        selected = 0;
        return;
    }

    int start = selected;
    while(start < 0) start += count;
    while(start >= count) start -= count;

    if(motor_menu_item_visible(menuNum, static_cast<uint8_t>(start))) {
        selected = static_cast<int8_t>(start);
        return;
    }

    for(uint8_t offset = 1; offset < count; ++offset) {
        const uint8_t idx = static_cast<uint8_t>((start + offset) % count);
        if(motor_menu_item_visible(menuNum, idx)) {
            selected = static_cast<int8_t>(idx);
            return;
        }
    }

    selected = 0;
}

void moveVisibleMenuSelection(uint8_t menuNum, int8_t& selected, int delta) {
    const uint8_t count = menuItemCount(menuNum);
    if(count == 0) {
        selected = 0;
        return;
    }

    ensureVisibleMenuSelection(menuNum, selected);
    if(delta == 0) {
        return;
    }

    const int direction = delta > 0 ? 1 : -1;
    int remaining = abs(delta);
    int current = selected;
    while(remaining-- > 0) {
        for(uint8_t guard = 0; guard < count; ++guard) {
            current += direction;
            if(current < 0) current = count - 1;
            if(current >= count) current = 0;
            if(motor_menu_item_visible(menuNum, static_cast<uint8_t>(current))) {
                break;
            }
        }
    }

    selected = static_cast<int8_t>(current);
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
        case DRIVE_TYPE_KRS_130ST_M07725:
            motorSelectSelected = MOTOR_SELECT_KRS_130ST_M07725;
            break;
        case DRIVE_TYPE_KRS_XB130AEA20025_SHL:
            motorSelectSelected = MOTOR_SELECT_KRS_XB130AEA20025_SHL;
            break;
        case DRIVE_TYPE_SEW_MOVILINK:
            motorSelectSelected = MOTOR_SELECT_SEW_MOVILINK;
            break;
        case DRIVE_TYPE_TSE:
            motorSelectSelected = MOTOR_SELECT_TSE;
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
            motor_force_drive_type(DRIVE_TYPE_JMC, true);
            break;
        case MOTOR_SELECT_RT:
            motor_force_drive_type(DRIVE_TYPE_RT, true);
            break;
        case MOTOR_SELECT_KRS_130ST_M07725:
            motor_force_drive_type(DRIVE_TYPE_KRS_130ST_M07725, true);
            break;
        case MOTOR_SELECT_KRS_XB130AEA20025_SHL:
            motor_force_drive_type(DRIVE_TYPE_KRS_XB130AEA20025_SHL, true);
            break;
        case MOTOR_SELECT_SEW_MOVILINK:
            motor_force_drive_type(DRIVE_TYPE_SEW_MOVILINK, true);
            break;
        case MOTOR_SELECT_TSE:
            motor_force_drive_type(DRIVE_TYPE_TSE, true);
            break;
        case MOTOR_SELECT_AUTO:
        default:
            pdata_write_drive_type(DRIVE_TYPE_UNKNOWN);
            motor_redetect_drive(false);
            break;
    }

    error = 0;
    errorCode = 0;
    commCounter = 0;
    controller_note_activity();
    state = motorSelectReturnState;
    display_show_action_feedback("Drive applied");
}

States networkInfoReturnState = MENU1;
States aboutInfoReturnState = MENU1;

void syncWirelessMenuFromMain() {
    Menu5[Menu5Items::WIRELESS_RADIO_MODE].value = Menu1[Menu1Items::RADIOMODE].value;
}

void openWirelessMenu() {
    syncWirelessMenuFromMain();
    enterMenu = false;
    menu5Selected = Menu5Items::RETURN_FROM_WIRELESS;
    state = MENU5;
    disp_updateRequired = true;
}

void openNetworkInfo(States returnState) {
    networkInfoReturnState = returnState;
    enterMenu = false;
    state = NETWORK_INFO;
    disp_updateRequired = true;
}

void openAboutInfo(States returnState) {
    aboutInfoReturnState = returnState;
    enterMenu = false;
    state = ABOUT_INFO;
    disp_updateRequired = true;
}

int16_t gaugeMenuValueFromUm(int32_t gapUm) {
    int32_t steps = gapUm / 10;
    if(steps > Menu1[Menu1Items::GAUGESET].maxValue) steps = Menu1[Menu1Items::GAUGESET].maxValue;
    if(steps < Menu1[Menu1Items::GAUGESET].minValue) steps = Menu1[Menu1Items::GAUGESET].minValue;
    return static_cast<int16_t>(steps);
}

int32_t gaugeMenuValueToUm(int16_t value) {
    return static_cast<int32_t>(value) * 10;
}

void syncGaugeSetMenuValue() {
    Menu1[Menu1Items::GAUGESET].value = gaugeMenuValueFromUm(burr_gap_base_lcd_um());
}
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
ESP32Encoder encoder;

int16_t storedSetRPM = 0;
uint32_t storedSetWeight = 0;

void io_init() { 
    const ControllerBoardPins& pins = controller_board_pins();

    if(controller_board_has_start_button() && controller_board_pin_valid(pins.startButton)) {
        pinMode(pins.startButton, INPUT_PULLUP);
        portENTER_CRITICAL(&buttonMux);
        startButtonAvailable = true;
        startButtonIdleLevel = digitalRead(pins.startButton);
        startButtonLastRawLevel = startButtonIdleLevel;
        startButtonPressed = false;
        startButtonPressEvent = false;
        startButtonReleaseEvent = false;
        startButtonDebounceStartedAt = millis();
        portEXIT_CRITICAL(&buttonMux);
    } else {
        portENTER_CRITICAL(&buttonMux);
        startButtonAvailable = false;
        startButtonIdleLevel = true;
        startButtonLastRawLevel = true;
        startButtonPressed = false;
        startButtonPressEvent = false;
        startButtonReleaseEvent = false;
        startButtonDebounceStartedAt = millis();
        portEXIT_CRITICAL(&buttonMux);
    }

    bool encoderIdleLevel = true;
    if(controller_board_has_encoder_button() && controller_board_pin_valid(pins.encoderButton)) {
        pinMode(pins.encoderButton, INPUT_PULLUP);
        encoderIdleLevel = digitalRead(pins.encoderButton);
    }
    portENTER_CRITICAL(&buttonMux);
    encButtonIdleLevel = encoderIdleLevel;
    encButtonLastRawLevel = encoderIdleLevel;
    encButtonPressed = false;
    encButtonChanged = false;
    encButtonPressEvent = false;
    encButtonLongPressLatched = false;
    encButtonShortReleaseEvent = false;
    encButtonLongPressEvent = false;
    encButtonDebounceStartedAt = millis();
    encButtonLastStableChangeAt = encButtonDebounceStartedAt;
    portEXIT_CRITICAL(&buttonMux);

    if(controller_board_has_start_led() && controller_board_pin_valid(pins.startButtonLed)) {
        pinMode(pins.startButtonLed, OUTPUT);
        ledcAttach(pins.startButtonLed, 2000, 10);
        ledcWrite(pins.startButtonLed, 0);
    }

    if(controller_board_has_encoder() &&
       controller_board_pin_valid(pins.encoderA) &&
       controller_board_pin_valid(pins.encoderB)) {
        pinMode(pins.encoderA, INPUT);
        pinMode(pins.encoderB, INPUT);
        ESP32Encoder::useInternalWeakPullResistors = puType::up;
        encoder.attachHalfQuad(pins.encoderA, pins.encoderB);
        encoder.setFilter(100);
        encoder.setCount(0);
    }

    startButtonServiceTask();
}

void do_io() { 
    static bool lastIdle = false; //lastIdle true means it was on normal Idle before, false means idle_GBW
    static int16_t OriginalValue = 0;
    static bool notReleasedYet = false;

    if(state == IDLE) { 
        controller_idle_sleep_active() ? ledAction(0) : ledAction(3);
        setRPM += encoder_change()*rpm_scalar;

        if(setRPM > Menu1[SETMAX].value) setRPM = Menu1[SETMAX].value;
        if(setRPM < Menu1[SETMIN].value) setRPM = Menu1[SETMIN].value;
        
        //START BUTTON
        if(START_BUTTON() == true && dimmed == false) {
            if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
                motor_ensure_drive_detected(true);
            }
            if(shouldOpenDriveSelector()) openDriveSelector(IDLE);
            else if(!showStartBlockedIfNeeded()) {
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
            if(motor_get_drive_type() == DRIVE_TYPE_UNKNOWN) {
                motor_ensure_drive_detected(true);
            }
            if(shouldOpenDriveSelector()) openDriveSelector(IDLE_GBW);
            else if(!showStartBlockedIfNeeded()) state = GRINDING_GBW;
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
            const int delta = Menu1[INVERT_SCROLL].value ? encoder_change() : -encoder_change();
            moveVisibleMenuSelection(1, menu1Selected, delta);
            ledAction(0);
        } else {
            ensureVisibleMenuSelection(1, menu1Selected);
        }

        if(enterMenu == true) { 
            const int encoderDelta = encoder_change();
            if(menu1Selected == Menu1Items::RADIOMODE) {
                Menu1[menu1Selected].value = radio_mode_next_user_value(Menu1[menu1Selected].value, encoderDelta);
            } else {
                Menu1[menu1Selected].value += encoderDelta * Menu1[menu1Selected].scalar;
            }
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
            ensureVisibleMenuSelection(1, menu1Selected);
            if(enterMenu == false) { 
                if(menu1Selected == Menu1Items::PURGESETTINGS) state = MENU2;
                if(menu1Selected == Menu1Items::GBWSETTINGS) state = MENU3;
                if(menu1Selected == Menu1Items::JMCBAUDMENU) {
                    if(motor_jmc_tools_supported()) {
                        state = MENU4;
                    } else {
                        display_show_action_feedback("JMC only");
                    }
                }
                if(menu1Selected == Menu1Items::DRIVESETTINGS) openDriveSelector(MENU1);
                if(menu1Selected == Menu1Items::RADIOMODE) {
                    openWirelessMenu();
                }
                if(menu1Selected == Menu1Items::NETWORKINFO) {
                    openNetworkInfo(MENU1);
                }
                if(menu1Selected == Menu1Items::ABOUT) {
                    openAboutInfo(MENU1);
                }
                if(menu1Selected == Menu1Items::EXITMENU) lastIdle? state = IDLE : state = IDLE_GBW; 
                if(menu1Selected == Menu1Items::GAUGESET) {
                    syncGaugeSetMenuValue();
                }
                if(menu1Selected != Menu1Items::PURGESETTINGS &&
                   menu1Selected != Menu1Items::GBWSETTINGS &&
                   menu1Selected != Menu1Items::JMCBAUDMENU &&
                   menu1Selected != Menu1Items::DRIVESETTINGS &&
                   menu1Selected != Menu1Items::RADIOMODE &&
                   menu1Selected != Menu1Items::NETWORKINFO &&
                   menu1Selected != Menu1Items::ABOUT &&
                   menu1Selected != Menu1Items::EXITMENU) enterMenu = true; //entering settings adjustment
                OriginalValue = Menu1[menu1Selected].value;
            } else if(menu1Selected == Menu1Items::CALIBRATE) {
                startCalibrationFromMenu(notReleasedYet);
            } else if(menu1Selected == Menu1Items::GAUGESET) {
                enterMenu = false;
                burr_gap_set_base_um(gaugeMenuValueToUm(Menu1[Menu1Items::GAUGESET].value));
                display_show_action_feedback("Gap set");
            } else { 
                enterMenu = false;
                if(menu1Selected == Menu1Items::RADIOMODE) {
                    if(radio_mode_apply()) {
                        pdata_write(2);
                        display_show_action_feedback("Wi-Fi applied");
                    } else {
                        Menu1[menu1Selected].value = OriginalValue;
                        disp_updateRequired = true;
                    }
                } else if(menu1Selected != Menu1Items::RESET && menu1Selected != Menu1Items::RESETNETWORK) {
                    pdata_write(2);
                    if(menu1Selected != Menu1Items::CALIBRATE) {
                        showMenuEntryFeedback(Menu1[menu1Selected]);
                    }
                }
                pdata_normalize();
                if(menu1Selected == Menu1Items::RESETNETWORK && Menu1[menu1Selected].value == 1) {
                    wifi_ui_reset_network_settings();
                    Menu1[menu1Selected].value = 0;
                    display_show_action_feedback("Network reset");
                    disp_updateRequired = true;
                }
                if(menu1Selected == Menu1Items::RESET && Menu1[menu1Selected].value == 1) { 
                    display_show_action_feedback("Defaults reset", 1000);
                    delay(650);
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
            startCalibrationFromMenu(notReleasedYet);
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
                showMenuEntryFeedback(Menu2[menu2Selected]);
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
                if(menu3Selected == Menu3Items::SAVE_SCALE) {
                    display_show_action_feedback(Menu3[SAVE_SCALE].value == 0 ? "Scale reset" : "Scale applied");
                } else {
                    showMenuEntryFeedback(Menu3[menu3Selected]);
                }
            
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

    if(state == MENU5) {
        ledAction(0);
        if(enterMenu == false) {
            syncWirelessMenuFromMain();
            const int delta = Menu1[INVERT_SCROLL].value ? encoder_change() : -encoder_change();
            moveVisibleMenuSelection(5, menu5Selected, delta);
        } else {
            ensureVisibleMenuSelection(5, menu5Selected);
            if(menu5Selected == Menu5Items::WIRELESS_RADIO_MODE) {
                Menu5[menu5Selected].value = radio_mode_next_user_value(Menu5[menu5Selected].value, encoder_change());
            } else {
                (void)encoder_change();
            }
        }

        if(START_BUTTON() == true) {
            if(enterMenu == false) {
                menu5Selected = Menu5Items::RETURN_FROM_WIRELESS;
                state = MENU1;
            } else {
                enterMenu = false;
                syncWirelessMenuFromMain();
            }
        }

        if(ENC_BUTTON_DOWN() == true) {
            if(enterMenu == false) {
                if(menu5Selected == Menu5Items::RETURN_FROM_WIRELESS) {
                    state = MENU1;
                } else if(menu5Selected == Menu5Items::WIRELESS_RADIO_MODE) {
                    enterMenu = true;
                    OriginalValue = Menu5[menu5Selected].value;
                } else if(menu5Selected == Menu5Items::WIRELESS_NETWORK_INFO) {
                    openNetworkInfo(MENU5);
                } else if(menu5Selected == Menu5Items::WIRELESS_RESET_NETWORK) {
                    wifi_ui_reset_network_settings();
                    Menu1[Menu1Items::RESETNETWORK].value = 0;
                    display_show_action_feedback("Network reset");
                    disp_updateRequired = true;
                }
            } else {
                enterMenu = false;
                if(menu5Selected == Menu5Items::WIRELESS_RADIO_MODE) {
                    const int16_t originalValue = Menu1[Menu1Items::RADIOMODE].value;
                    Menu1[Menu1Items::RADIOMODE].value = Menu5[menu5Selected].value;
                    if(radio_mode_apply()) {
                        pdata_write(2);
                        display_show_action_feedback("Wireless applied");
                    } else {
                        Menu1[Menu1Items::RADIOMODE].value = originalValue;
                        syncWirelessMenuFromMain();
                        disp_updateRequired = true;
                    }
                }
                pdata_normalize();
            }
        }
    }

    if(state == NETWORK_INFO || state == ABOUT_INFO) {
        ledAction(0);
        (void)encoder_change();
        static unsigned long lastInfoRefreshAt = 0;
        if(lastInfoRefreshAt == 0 || millis() - lastInfoRefreshAt >= 1000) {
            lastInfoRefreshAt = millis();
            disp_updateRequired = true;
        }

        if(START_BUTTON() == true) {
            controller_ota_local_cancel();
            state = state == NETWORK_INFO ? networkInfoReturnState : aboutInfoReturnState;
            disp_updateRequired = true;
        } else if(ENC_BUTTON_DOWN() == true) {
            if(state == ABOUT_INFO) {
                controller_ota_local_confirm();
            } else {
                state = networkInfoReturnState;
                disp_updateRequired = true;
            }
        }
    }

    if(state == MENU4) {
        if(!motor_jmc_tools_supported()) {
            enterMenu = false;
            menu4Selected = Menu4Items::RETURN_FROM_JMC_BAUD;
            state = MENU1;
            disp_updateRequired = true;
            display_show_action_feedback("JMC only");
            return;
        }

        ledAction(0);
        if(enterMenu == false) {
            const int delta = Menu1[INVERT_SCROLL].value ? encoder_change() : -encoder_change();
            moveVisibleMenuSelection(4, menu4Selected, delta);
        } else {
            ensureVisibleMenuSelection(4, menu4Selected);
        }

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
                enterMenu = false;
                motor_run_jmc_baud_switch(baudForJmcMenuItem(menu4Selected));
                display_show_action_feedback("Baud applied");
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
    bool event = false;
    portENTER_CRITICAL(&buttonMux);
    if(encButtonLongPressEvent) {
        encButtonLongPressEvent = false;
        encButtonPressEvent = false;
        event = true;
    }
    portEXIT_CRITICAL(&buttonMux);
    if(event) controller_note_activity();
    return event;
}

int16_t encoder_change() { 
    if(!controller_board_has_encoder()) {
        return 0;
    }

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
}

bool check_io() { //returns true immediately when a button is pressed - normal control uses wasReleased to prevent double feedback.
    portENTER_CRITICAL(&buttonMux);
    const bool active = encButtonPressed ||
                        encButtonPressEvent ||
                        encButtonShortReleaseEvent ||
                        encButtonLongPressEvent ||
                        startButtonPressed ||
                        startButtonPressEvent;
    portEXIT_CRITICAL(&buttonMux);
    return active;
}

bool ENC_BUTTON_DOWN() {
    bool event = false;
    portENTER_CRITICAL(&buttonMux);
    if(encButtonPressEvent) {
        encButtonPressEvent = false;
        encButtonLongPressLatched = true;
        event = true;
    }
    portEXIT_CRITICAL(&buttonMux);
    if(event) controller_note_activity();
    return event;
}

bool ENC_BUTTON() { 
    bool event = false;
    portENTER_CRITICAL(&buttonMux);
    if(encButtonShortReleaseEvent) {
        encButtonShortReleaseEvent = false;
        encButtonPressEvent = false;
        event = true;
    }
    portEXIT_CRITICAL(&buttonMux);
    if(event) controller_note_activity();
    return event;
}

bool START_BUTTON() {
    bool event = false;
    portENTER_CRITICAL(&buttonMux);
    if(startButtonAvailable && startButtonPressEvent) {
        startButtonPressEvent = false;
        startButtonReleaseEvent = false;
        event = true;
    }
    portEXIT_CRITICAL(&buttonMux);
    if(event) controller_note_activity();
    return event;
}

void ledAction(uint8_t type) {  //Type: 0 = off, 1 = on, 2 = flashing, 3 = fading, 4 flashing fast, 5 = flashing fastest, 100-200 = %.
    if(!controller_board_has_start_led()) {
        (void)type;
        return;
    }

    const ControllerBoardPins& pins = controller_board_pins();
    static unsigned long lastCall;
    static uint8_t lastType;
    static unsigned long lastFlash;
    static uint16_t lastLedValue = 0xFFFF;
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

        ledcWrite(pins.startButtonLed, ledValue*(float(Menu1[LED_BRIGHTNESS].value)/100));
        lastLedValue = ledValue;
    }

    lastType = type;
}
