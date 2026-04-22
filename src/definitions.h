// pin definitions and settings
#include <Arduino.h>
#pragma once

// Motor type is selected at runtime through the HAL layer.

//DEBUG FLAGS 
// #define DEBUG
#define DEBUG_SCALE false
// #define DEBUG_COMM
// #define DEBUG_MOTORCONTROL
// #define DEBUG_CALIB
// #define DEBUG_GBW

//#define BUILD_ID (new build ID = resets all saved params)
#define BUILD_ID "0002"

// USER SETTINGS
#define absolute_min_rpm 0
#define MOTOR_MAX_RPM_JMC 2000
#define MOTOR_MAX_RPM_RT 3000
#define absolute_max_rpm MOTOR_MAX_RPM_RT

#define rpm_scalar 25
#define SIZEOFCALIBRATEARRAY (2*(absolute_max_rpm - absolute_min_rpm)/rpm_scalar)
#define LONGPRESS 750 // after Xms a long press is registered
#define DISP_REFRESH_RATE 50 //Hz

#if defined(SG98_CONTROLLER_BOARD_WAVESHARE_RS485_CAN)
#define SG98_CONTROLLER_TARGET_NAME "waveshare_rs485_can"
#define SG98_HAS_DISPLAY 0
#define SG98_HAS_START_BUTTON 0
#define SG98_HAS_START_LED 0
#define SG98_HAS_ENCODER 0
#define SG98_HAS_ENCODER_BUTTON 0

// COMM PINS
#define RS485_RXD 18
#define RS485_TXD 17
// Waveshare ties /RE and DE to a shared RS485_EN line.
#define RS485RE 21
#define RS485DE 21

// IO PINS
#define BTN 0
#define start_button_led (-1)
#define ENC_BTN (-1)
#define ENC_A (-1)
#define ENC_B (-1)

// No onboard local display on this controller target.
#define DISP_SCL (-1)
#define DISP_SDA (-1)
#define DISP_RST (-1)
#define DISP_DC (-1)
#define DISP_CS (-1)
#define DISP_BL (-1)

// EXIO header pins exposed on the board.
#define EXSPI_MOSI (-1)
#define EXSPI_MISO (-1)
#define EXSPI_CLK (-1)
#define EXPSI_CS (-1)
#define EXSDA 1
#define EXSCL 2

// These must be the same as below!
#define WAKE_BTN GPIO_NUM_0
#define WAKE_ENC_BTN GPIO_NUM_0
#define WAKE_ENC_A GPIO_NUM_0
#define WAKE_ENC_B GPIO_NUM_0
#define SLEEP_RE GPIO_NUM_21
#define SLEEP_DE GPIO_NUM_21
#else
#define SG98_CONTROLLER_TARGET_NAME "controller_box"
#define SG98_HAS_DISPLAY 1
#define SG98_HAS_START_BUTTON 1
#define SG98_HAS_START_LED 1
#define SG98_HAS_ENCODER 1
#define SG98_HAS_ENCODER_BUTTON 1

// COMM PINS
#define RS485_RXD 18 
#define RS485_TXD 17
#define RS485RE 15
#define RS485DE 16

// IO PINS
#define BTN 4
#define start_button_led 5
#define ENC_BTN 42
#define ENC_A 41
#define ENC_B 40

// SPI display PINS
#define DISP_SCL 10
#define DISP_SDA 11
#define DISP_RST 12
#define DISP_DC 13
#define DISP_CS 14
#define DISP_BL 21

//EX_SPI (unused but accessible in the board)
#define EXSPI_MOSI 35
#define EXSPI_MISO 37
#define EXSPI_CLK 36
#define EXPSI_CS 39

//EX I2C (unused but accessible)
#define EXSDA 8
#define EXSCL 9

// These must be the same as below!
#define WAKE_BTN GPIO_NUM_4
#define WAKE_ENC_BTN GPIO_NUM_42
#define WAKE_ENC_A GPIO_NUM_41
#define WAKE_ENC_B GPIO_NUM_40
#define SLEEP_RE GPIO_NUM_15
#define SLEEP_DE GPIO_NUM_16
#endif

// COMM SETTINGS
#define COMMINTERVAL 60 // comm interval during normal operation
#define COMM_DELAY_RECEIVE 50 //expect a respond 10ms after request send
#define COMM_DELAY_IDLE 1000
#define COMM_DELAY_SEND 60 //send every 50ms max
#define DISCONNECTED_AFTER 10000 //after Xms of not receiving comms, disconnected error shows
#define SEND_RPM_EVERY 5 //send RPM every X frames

//Encoder ticks per physical tick
#define ENC_TOL 1

//led behaviour
#define fadeTime 2500
#define flashTime 250
#define fastFlashTime 150
#define fastestFlashTime 75

// defaults
#define default_sleepTime 120 //in s
#define CAL_TIME 2500 //calibration time minus 500ms of settling in
#define default_setRPM 1000
#define default_brightness 200
#define default_maxRPM 2000
#define default_minRPM 100
#define default_motor_torque 100
#define default_led_brightness 100
#define default_radio_mode 0
#define default_espnow_channel 1

#define default_motor_ramp 1
#define default_motor_direction_reversed 0

#define default_setWeight 18000
#define default_max_weight 99900
#define default_min_weight 6000
#define default_GBWRPM 800
#define default_slow_mg 4500
#define default_slow_rpm 300
#define default_speedModifier 2500
#define default_start_delay 100
#define default_time_offset 150
#define default_auto_off_time 60

#define default_autoPurgeEnabled 1
#define default_buttonPurge 1
#define default_autoPurgeEnabled 1
#define default_auto_off_time 60

#define default_purgeFramesLow 7
#define default_purgeFramesHigh 5
#define default_purgePrctLow 130
#define default_purgePrctHigh 130

#define default_purgeDuration 3000
#define default_purgeDelay 2000
#define default_purgeForwardRPM 2000
#define default_reverseRotation 0
#define default_purgeStabilTime 500
#define default_purge_off_time 1000
