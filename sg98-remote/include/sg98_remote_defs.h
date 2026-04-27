#pragma once

// Shared SG98 menu and default constants used by the standalone remote project.

#define absolute_min_rpm 0
#define MOTOR_MAX_RPM_JMC 2000
#define MOTOR_MAX_RPM_RT 3000
#define absolute_max_rpm MOTOR_MAX_RPM_RT

#define rpm_scalar 25
#define LONGPRESS 750

#define default_sleepTime 5
#define default_setRPM 1000
#define default_brightness 200
#define default_maxRPM 2000
#define default_minRPM 100
#define default_motor_torque 100
#define default_led_brightness 100
#define default_radio_mode 1
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

enum Menu1Items {
    EXITMENU,
    PURGESETTINGS,
    GBWSETTINGS,
    SETMAX,
    SETMIN,
    SETMOTORTORQUE,
    SETMOTORRAMP,
    SETMOTORDIRECTION,
    DRIVESETTINGS,
    JMCBAUDMENU,
    CALIBRATE,
    RADIOMODE,
    RESETNETWORK,
    SETSLEEP,
    DISP_BRIGHTNESS,
    LED_BRIGHTNESS,
    INVERT_SCROLL,
    RESET,
    NETWORKINFO,
    NUM_MENU1_ITEMS
};

enum Menu2Items {
    RETURN_FROM_PURGE,
    AUTO_PURGE_ENABLED,
    SETBUTTONPURGE,
    SETPURGERPM,
    SETPURGEREVERSE,
    SETPURGEDELAY,
    SETPURGETIME,
    SETPURGEOFFTIME,
    SETPURGESTABILTIME,
    SETPURGEFRAMESHIGH,
    SETPURGEFRAMESLOW,
    SETPURGEPRCTHIGH,
    SETPURGEPRCTLOW,
    AUTO_OFF_TIME,
    NUM_MENU2_ITEMS
};

enum Menu3Items {
    RETURN_FROM_GBW,
    GBW_RPM_SET,
    GBW_SLOW_RPM,
    GBW_SLOW_MG,
    GBW_BUTTON_DELAY,
    GBW_SPEEDMOD,
    GBW_OFFSET,
    SAVE_SCALE,
    NUM_MENU3_ITEMS
};

enum Menu4Items {
    RETURN_FROM_JMC_BAUD,
    JMCBAUD9600,
    JMCBAUD19200,
    JMCBAUD38400,
    JMCBAUD57600,
    JMCBAUD115200,
    NUM_MENU4_ITEMS
};

enum Menu5Items {
    RETURN_FROM_WIRELESS,
    WIRELESS_RADIO_MODE,
    WIRELESS_NETWORK_INFO,
    WIRELESS_RESET_NETWORK,
    NUM_MENU5_ITEMS
};
