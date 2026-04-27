#include "definitions.h"

#ifndef display_h
#define display_h

extern bool dimmed;
extern bool disp_updateRequired;
extern unsigned long drawnErrorAt;

void display_init();
void setBrightness();
void update_display();
void display_off();
void display_on();
void display_show_motor_probe_splash(const char* motorLabel);
void display_show_action_feedback(const char* label, unsigned long durationMs = 900);

void drawErrorOverlay();
void drawIdleScreen();
void drawGbWIdleScreen();
void drawGrindingOrPurgingScreen();
void drawGrindingGbwScreen();
void drawCalibratingScreen();
void drawMenuScreen(int menuNum, int numItems, int selectedIdx);
void drawMenuValueScreen(int menuNum, int selectedIdx);
void drawMotorSelectScreen();


#endif
