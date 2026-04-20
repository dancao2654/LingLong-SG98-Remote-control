#include "definitions.h"

#ifndef pdata_h
#define pdata_h

#include <Preferences.h>
void pdata_init();
void pdata_write(uint8_t what);
void pdata_read();
void pdata_normalize(bool persist = true);
uint32_t pdata_read_jmc_baud();
void pdata_write_jmc_baud(uint32_t baud_rate);

#endif
