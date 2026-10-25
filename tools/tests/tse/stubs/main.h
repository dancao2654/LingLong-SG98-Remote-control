#pragma once
#include <cstdint>
enum States { IDLE, GRINDING, MENU1 };
extern States state;
extern uint8_t error;
extern uint16_t errorCode;
extern bool newData;
bool controller_is_menu_state(States value);
