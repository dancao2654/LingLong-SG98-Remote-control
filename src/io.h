#include "definitions.h"

#ifndef IO_H
#define IO_H

void io_init();
bool check_io();
void ledAction(uint8_t type); //Type: 0 = off, 1 = on, 2 = flashing, 3 = fading, 4 flashing fast, 5 = flashing fastest

int16_t encoder_change();

void do_io();

bool START_BUTTON();
bool ENC_BUTTON();
bool ENC_BUTTON_DOWN();
bool pressedLong();

extern int16_t storedSetRPM;
extern uint32_t storedSetWeight;

#endif
