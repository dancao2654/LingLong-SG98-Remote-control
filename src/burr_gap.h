#pragma once

#include <Arduino.h>

void burr_gap_init();
void burr_gap_poll();

bool burr_gap_enabled();
bool burr_gap_has_value();
bool burr_gap_live();
bool burr_gap_recently_tared();

int32_t burr_gap_display_um();
int32_t burr_gap_raw_um();
int32_t burr_gap_offset_um();
uint32_t burr_gap_last_frame_ms();

void burr_gap_set_current_gap_um(int32_t gapUm);
