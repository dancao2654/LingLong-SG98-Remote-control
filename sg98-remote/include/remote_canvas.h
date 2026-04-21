#pragma once

#include <Arduino.h>

enum class RemoteTextStyle : uint8_t {
    Tiny,
    Small,
    Medium,
    Large,
    Hero,
};

class RemoteCanvas {
  public:
    virtual ~RemoteCanvas() = default;

    virtual uint16_t width() const = 0;
    virtual uint16_t height() const = 0;

    virtual void fillScreen(uint16_t color) = 0;
    virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;
    virtual void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;
    virtual void fillRoundRect(int16_t x,
                               int16_t y,
                               int16_t w,
                               int16_t h,
                               int16_t radius,
                               uint16_t color) = 0;
    virtual void drawRoundRect(int16_t x,
                               int16_t y,
                               int16_t w,
                               int16_t h,
                               int16_t radius,
                               uint16_t color) = 0;
    virtual void fillCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) = 0;
    virtual void drawCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) = 0;
    virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) = 0;

    virtual void drawCenteredText(const String& text,
                                  int16_t x,
                                  int16_t y,
                                  RemoteTextStyle style,
                                  uint16_t textColor,
                                  uint16_t bgColor) = 0;
    virtual void drawTextTopLeft(const String& text,
                                 int16_t x,
                                 int16_t y,
                                 RemoteTextStyle style,
                                 uint16_t textColor,
                                 uint16_t bgColor) = 0;
    virtual void drawTextTopRight(const String& text,
                                  int16_t x,
                                  int16_t y,
                                  RemoteTextStyle style,
                                  uint16_t textColor,
                                  uint16_t bgColor) = 0;
};
