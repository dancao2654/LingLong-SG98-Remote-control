#pragma once

#include "../Arduino_GFX.h"
#include "../Arduino_TFT.h"

#define RM690B0_TFTWIDTH 600
#define RM690B0_TFTHEIGHT 450

#define RM690B0_RST_DELAY 120
#define RM690B0_SLPIN_DELAY 120
#define RM690B0_SLPOUT_DELAY 120

#define RM690B0_SWRESET 0x01

#define RM690B0_SLPIN 0x10
#define RM690B0_SLPOUT 0x11

#define RM690B0_INVOFF 0x20
#define RM690B0_INVON 0x21

#define RM690B0_DISPOFF 0x28
#define RM690B0_DISPON 0x29

#define RM690B0_CASET 0x2A
#define RM690B0_PASET 0x2B
#define RM690B0_RAMWR 0x2C
#define RM690B0_RAMRD 0x2E

#define RM690B0_MADCTL 0x36
#define RM690B0_PIXFMT 0x3A
#define RM690B0_BRIGHTNESS 0x51

#define RM690B0_MADCTL_MY 0x80
#define RM690B0_MADCTL_MX 0x40
#define RM690B0_MADCTL_MV 0x20
#define RM690B0_MADCTL_ML 0x10
#define RM690B0_MADCTL_RGB 0x00
#define RM690B0_MADCTL_MH 0x04
#define RM690B0_MADCTL_BGR 0x08

static const uint8_t rm690b0_init_operations[] = {
    BEGIN_WRITE,
    WRITE_C8_D8, 0xFE, 0x20,
    WRITE_C8_D8, 0x26, 0x0A,
    WRITE_C8_D8, 0x24, 0x80,
    WRITE_C8_D8, 0x5A, 0x51,
    WRITE_C8_D8, 0x5B, 0x2E,
    WRITE_C8_D8, 0xFE, 0x00,
    WRITE_C8_D8, RM690B0_PIXFMT, 0x55,
    WRITE_COMMAND_8, 0xC2,
    END_WRITE,
    DELAY, 10,
    BEGIN_WRITE,
    WRITE_C8_D8, 0x35, 0x00,
    WRITE_C8_D8, RM690B0_BRIGHTNESS, 0x00,
    WRITE_COMMAND_8, RM690B0_SLPOUT,
    END_WRITE,
    DELAY, RM690B0_SLPOUT_DELAY,
    BEGIN_WRITE,
    WRITE_COMMAND_8, RM690B0_DISPON,
    END_WRITE,
    DELAY, 10,
    BEGIN_WRITE,
    WRITE_C8_D8, RM690B0_BRIGHTNESS, 0xD0,
    END_WRITE};

class Arduino_RM690B0 : public Arduino_TFT
{
public:
  Arduino_RM690B0(
      Arduino_DataBus *bus, int8_t rst, uint8_t r,
      int16_t w = RM690B0_TFTWIDTH, int16_t h = RM690B0_TFTHEIGHT,
      uint8_t col_offset1 = 0, uint8_t row_offset1 = 0, uint8_t col_offset2 = 0, uint8_t row_offset2 = 0);

  bool begin(int32_t speed = GFX_NOT_DEFINED) override;

  void setRotation(uint8_t r) override;
  void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override;
  void invertDisplay(bool) override;
  void displayOn() override;
  void displayOff() override;
  void setBrightness(uint8_t brightness);

protected:
  void tftInit() override;
};
