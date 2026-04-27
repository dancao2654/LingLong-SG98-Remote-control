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

// COMM SETTINGS
#define COMMINTERVAL 60 // comm interval during normal operation
#define COMM_DELAY_RECEIVE 50 //expect a respond 10ms after request send
#define COMM_DELAY_IDLE 1000
#define COMM_DELAY_MENU 1500
#define COMM_DELAY_SEND 60 //send every 50ms max
#define DISCONNECTED_AFTER 10000 //after Xms of not receiving comms, disconnected error shows
#define SEND_RPM_EVERY 5 //send RPM every X frames
#define HEADLESS_REMOTE_DEADMAN_MS 7500 // stop headless motor states if remote traffic disappears

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
#define default_radio_mode 3 // controller default: Web/AP + ESP-NOW
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
