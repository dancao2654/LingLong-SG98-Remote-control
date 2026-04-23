#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

enum class ControllerBoardId : uint8_t {
    Unknown = 0,
    AdafruitFeather,
    WaveshareRs485Can,
};

struct ControllerBoardFeatures {
    bool hasDisplay;
    bool hasStartButton;
    bool hasStartLed;
    bool hasEncoder;
    bool hasEncoderButton;
};

struct ControllerBoardPins {
    int8_t rs485Rxd;
    int8_t rs485Txd;
    int8_t rs485Re;
    int8_t rs485De;

    int8_t startButton;
    int8_t startButtonLed;
    int8_t encoderButton;
    int8_t encoderA;
    int8_t encoderB;

    int8_t displaySclk;
    int8_t displaySda;
    int8_t displayReset;
    int8_t displayDc;
    int8_t displayCs;
    int8_t displayBacklight;

    gpio_num_t wakeButton;
    gpio_num_t wakeEncoderButton;
    gpio_num_t wakeEncoderA;
    gpio_num_t wakeEncoderB;
    gpio_num_t sleepRe;
    gpio_num_t sleepDe;
};

struct ControllerBoardInfo {
    ControllerBoardId id;
    const char* targetName;
    const char* displayName;
    ControllerBoardFeatures features;
    ControllerBoardPins pins;
};

void controller_board_init();
bool controller_board_ready();
const ControllerBoardInfo& controller_board_info();
const ControllerBoardFeatures& controller_board_features();
const ControllerBoardPins& controller_board_pins();
const char* controller_board_target_name();
const char* controller_board_display_name();
uint32_t controller_board_detected_flash_size_bytes();

inline bool controller_board_pin_valid(int pin) {
    return pin >= 0;
}

inline bool controller_board_gpio_valid(gpio_num_t pin) {
    return pin >= GPIO_NUM_0 && pin < GPIO_NUM_MAX;
}

inline bool controller_board_has_display() {
    return controller_board_features().hasDisplay;
}

inline bool controller_board_has_start_button() {
    return controller_board_features().hasStartButton;
}

inline bool controller_board_has_start_led() {
    return controller_board_features().hasStartLed;
}

inline bool controller_board_has_encoder() {
    return controller_board_features().hasEncoder;
}

inline bool controller_board_has_encoder_button() {
    return controller_board_features().hasEncoderButton;
}
