#include "definitions.h"

#ifndef pdata_h
#define pdata_h

#include <Preferences.h>
#include <stddef.h>
void pdata_init();
void pdata_write(uint8_t what);
void pdata_read();
void pdata_normalize(bool persist = true);
uint32_t pdata_read_jmc_baud();
void pdata_write_jmc_baud(uint32_t baud_rate);
int16_t pdata_read_drive_type();
void pdata_write_drive_type(int16_t drive_type);
void pdata_read_smart_purge_learning(uint16_t* learnedLoad, uint16_t* confidence, size_t count);
void pdata_write_smart_purge_learning(const uint16_t* learnedLoad, const uint16_t* confidence, size_t count);
bool pdata_read_burr_gap_seen();
void pdata_write_burr_gap_seen(bool seen);
int32_t pdata_read_burr_gap_last_um();
void pdata_write_burr_gap_last_um(int32_t gapUm);
int32_t pdata_read_burr_gap_offset_um();
void pdata_write_burr_gap_offset_um(int32_t offsetUm);
bool pdata_has_burr_gap_manual_base();
bool pdata_read_burr_gap_manual_base();
void pdata_write_burr_gap_manual_base(bool manual);

#endif
