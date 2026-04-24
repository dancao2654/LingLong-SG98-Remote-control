#include "definitions.h"

#ifndef MAIN_H
#define MAIN_H

void vitals_init();
void doVitals();
void sleep_init();
void sleep_release();
void sleepTimeCheck();
bool controller_idle_sleep_active();
void controller_enter_idle_sleep();
void controller_exit_idle_sleep();
void controller_note_activity();
void checkPurge();
void do_purge();
void purge_prepare_for_grinding();
void purge_cancel_pending();
void purge_mark_started();

enum States{SLEEPING, IDLE, IDLE_GBW, GRINDING, PURGING, GRINDING_GBW, MENU1, MENU2, MENU3, MENU4, CALIBRATING, MOTOR_SELECT};

bool controller_is_menu_state(States currentState);

enum MotorSelectItems{
    MOTOR_SELECT_AUTO,
    MOTOR_SELECT_JMC,
    MOTOR_SELECT_RT,
    MOTOR_SELECT_KRS_130ST_M07725,
    NUM_MOTOR_SELECT_ITEMS
};

enum Menu1Items{
    EXITMENU,
    PURGESETTINGS,
    GBWSETTINGS,
    JMCBAUDMENU,
    RADIOMODE,
    SETSLEEP,
    DISP_BRIGHTNESS,
    LED_BRIGHTNESS,
    INVERT_SCROLL,
    SETMAX,
    SETMIN,
    SETMOTORTORQUE,
    SETMOTORRAMP,
    SETMOTORDIRECTION,
    CALIBRATE,
    RESETNETWORK,
    RESET,
    DRIVESETTINGS,
    NUM_MENU1_ITEMS
};

enum Menu2Items{
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

enum Menu3Items{
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

enum Menu4Items{
    RETURN_FROM_JMC_BAUD,
    JMCBAUD9600,
    JMCBAUD19200,
    JMCBAUD38400,
    JMCBAUD57600,
    JMCBAUD115200,
    NUM_MENU4_ITEMS
};

struct menuEntry { 
    const char* name;
    int16_t value;
    int16_t minValue;
    int16_t maxValue;
    int16_t scalar;
    const char* prefUID;
    const char* unit;
};

extern menuEntry Menu1[NUM_MENU1_ITEMS];
extern menuEntry Menu2[NUM_MENU2_ITEMS];
extern menuEntry Menu3[NUM_MENU3_ITEMS];
extern menuEntry Menu4[NUM_MENU4_ITEMS];

extern bool firstBootFlag;

extern States state;

extern bool modeGbW;
extern int8_t menu1Selected;
extern int8_t menu2Selected;
extern int8_t menu3Selected;
extern int8_t menu4Selected;
extern int8_t motorSelectSelected;
extern bool enterMenu;
extern States motorSelectReturnState;

extern int16_t setRPM;
extern uint32_t setWeight; //in mg (18g = 18000)

extern unsigned long lastActivity;

extern bool ready_purge;

/* errors: 0 = no error 
FATAL: 
1 = cant connect
2 = connection was present but failed 
3 = Error frame received / drive fault
4 = Comm discrepancy over 100
5 = Failed to create mutex
6 = motor stalled
7 = could not write all parameters
8 = first boot

NON FATAL: 
100 = motor set RPM not equal to read from controller
101 = calibration couldnt start due to... 
102 = calibration cancelled
103 = couldnt load preferences
104 = GbW scale disconnected during grinding
105 = GbW not producing any grounds after 3s
106 = GbW couldn't start -> no scale connected
107 = rigidity level changed, reboot drive


*/
extern volatile uint8_t error;
extern volatile bool newData;
extern uint16_t sleepTime;

#endif
