#pragma once

#include "driver/periph_ctrl.h"

#ifndef PERIPH_RCC_ATOMIC
#define PERIPH_RCC_ATOMIC() for(int _rc_cnt = 1; _rc_cnt; --_rc_cnt)
#endif
