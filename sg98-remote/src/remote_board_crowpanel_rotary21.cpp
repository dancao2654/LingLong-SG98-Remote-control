#include "remote_board_driver.h"

#include <ESP32Encoder.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

namespace {

constexpr uint16_t kDisplayWidth = 480;
constexpr uint16_t kDisplayHeight = 480;
constexpr uint16_t kColorBlack = 0x0000;

constexpr uint32_t kI2cClock = 100000U;
constexpr uint8_t kI2cAddressExpander = 0x21;
constexpr uint8_t kI2cAddressTouch = 0x15;

constexpr uint8_t kExpanderTouchReset = 0;
constexpr uint8_t kExpanderTouchInt = 2;
constexpr uint8_t kExpanderDisplayPower = 3;
constexpr uint8_t kExpanderDisplayReset = 4;
constexpr uint8_t kExpanderEncoderSwitch = 5;

constexpr uint32_t kButtonDebounceMs = 12;
constexpr uint32_t kLongPressMs = 750;
constexpr uint16_t kEncoderFilterUs = 40;
constexpr bool kDebugEncoderButton = false;
constexpr uint32_t kTouchProbeTimeoutMs = 900;
constexpr uint32_t kTouchProbeIntervalMs = 40;

constexpr uint8_t kBacklightPwmChannel = 2;
constexpr uint8_t kBacklightDefault = 200;

const RemoteBoardPins kBoardPins = [] {
    RemoteBoardPins pins = {};
    pins.touchSda = 38;
    pins.touchScl = 39;
    pins.displaySclk = 2;
    pins.displayMosi = 1;
    pins.displayCs = 16;
    pins.backlight = 6;
    pins.encoderA = 42;
    pins.encoderB = 4;
    return pins;
}();

class Pcf8574Io {
  public:
    bool begin(TwoWire* wire, uint8_t address) {
        wire_ = wire;
        address_ = address;
        return ping();
    }

    bool ping() const {
        if(wire_ == nullptr) {
            return false;
        }
        wire_->beginTransmission(address_);
        return wire_->endTransmission() == 0;
    }

    bool write(uint8_t value) const {
        if(wire_ == nullptr) {
            return false;
        }
        wire_->beginTransmission(address_);
        wire_->write(value);
        return wire_->endTransmission() == 0;
    }

    bool read(uint8_t* value) const {
        if(wire_ == nullptr || value == nullptr) {
            return false;
        }
        const size_t received = wire_->requestFrom(static_cast<int>(address_), 1);
        if(received != 1 || wire_->available() <= 0) {
            while(wire_->available()) {
                wire_->read();
            }
            return false;
        }

        *value = static_cast<uint8_t>(wire_->read());
        return true;
    }

  private:
    TwoWire* wire_ = nullptr;
    uint8_t address_ = 0;
};

class CrowPanelRotary21Touch {
  public:
    bool ping() const {
        Wire.beginTransmission(kI2cAddressTouch);
        return Wire.endTransmission() == 0;
    }

    bool readPoint(uint16_t* x, uint16_t* y) {
        uint8_t data[6] = {};
        if(!readRegisters(0x01, data, sizeof(data))) {
            return false;
        }

        const uint8_t touchPoints = data[1] & 0x0F;
        if(touchPoints == 0) {
            return false;
        }

        const uint16_t rawX = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
        const uint16_t rawY = static_cast<uint16_t>(((data[4] & 0x0F) << 8) | data[5]);
        if(rawX >= kDisplayWidth || rawY >= kDisplayHeight) {
            return false;
        }

        if(x != nullptr) *x = rawX;
        if(y != nullptr) *y = rawY;
        return true;
    }

  private:
    bool readRegisters(uint8_t reg, uint8_t* data, size_t len) {
        Wire.beginTransmission(kI2cAddressTouch);
        Wire.write(reg);
        if(Wire.endTransmission(false) != 0) {
            return false;
        }

        const size_t received = Wire.requestFrom(static_cast<int>(kI2cAddressTouch), static_cast<int>(len));
        if(received != len) {
            while(Wire.available()) {
                Wire.read();
            }
            return false;
        }

        for(size_t i = 0; i < len; ++i) {
            data[i] = static_cast<uint8_t>(Wire.read());
        }
        return true;
    }
};

class CrowPanelRotary21Panel : public lgfx::Panel_ST7701_Base {
  protected:
    const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr const uint8_t list0[] = {
            0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
            0xC0, 2, 0x3B, 0x00,
            0xC1, 2, 0x0B, 0x02,
            0xC2, 2, 0x00, 0x02,
            0xCC, 1, 0x10,
            0xCD, 1, 0x08,
            0xB0, 16, 0x02, 0x13, 0x1B, 0x0D, 0x10, 0x05, 0x08, 0x07,
                      0x07, 0x24, 0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D,
            0xB1, 16, 0x05, 0x13, 0x1B, 0x0D, 0x11, 0x05, 0x08, 0x07,
                      0x07, 0x24, 0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D,
            0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x11,
            0xB0, 1, 0x5D,
            0xB1, 1, 0x43,
            0xB2, 1, 0x81,
            0xB3, 1, 0x80,
            0xB5, 1, 0x43,
            0xB7, 1, 0x85,
            0xB8, 1, 0x20,
            0xC1, 1, 0x78,
            0xC2, 1, 0x78,
            0xD0, 1, 0x88,
            0xE0, 3, 0x00, 0x00, 0x02,
            0xE1, 11, 0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20,
            0xE2, 13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xE3, 4, 0x00, 0x00, 0x11, 0x00,
            0xE4, 2, 0x22, 0x00,
            0xE5, 16, 0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xE6, 4, 0x00, 0x00, 0x11, 0x00,
            0xE7, 2, 0x22, 0x00,
            0xE8, 16, 0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xEB, 7, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00,
            0xED, 16, 0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF,
                       0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF,
            0xEF, 6, 0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F,
            0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
            0xEF, 1, 0x08,
            0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x00,
            0x36, 1, 0x00,
            0x3A, 1, 0x60,
            0x11, 0 + CMD_INIT_DELAY, 100,
            0x29, 0 + CMD_INIT_DELAY, 50,
            0xFF, 0xFF,
        };

        switch(listno) {
            case 0: return list0;
            default: return nullptr;
        }
    }
};

class CrowPanelRotary21Display : public lgfx::LGFX_Device {
  public:
    CrowPanelRotary21Display() {
        {
            auto cfg = panel_.config();
            cfg.memory_width = kDisplayWidth;
            cfg.memory_height = kDisplayHeight;
            cfg.panel_width = kDisplayWidth;
            cfg.panel_height = kDisplayHeight;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            panel_.config(cfg);
        }

        {
            auto cfg = panel_.config_detail();
            cfg.pin_cs = kBoardPins.displayCs;
            cfg.pin_sclk = kBoardPins.displaySclk;
            cfg.pin_mosi = kBoardPins.displayMosi;
            panel_.config_detail(cfg);
        }

        {
            auto cfg = bus_.config();
            cfg.panel = &panel_;
            cfg.freq_write = 12000000;
            cfg.pin_d0 = 5;
            cfg.pin_d1 = 45;
            cfg.pin_d2 = 48;
            cfg.pin_d3 = 47;
            cfg.pin_d4 = 21;
            cfg.pin_d5 = 14;
            cfg.pin_d6 = 13;
            cfg.pin_d7 = 12;
            cfg.pin_d8 = 11;
            cfg.pin_d9 = 10;
            cfg.pin_d10 = 9;
            cfg.pin_d11 = 46;
            cfg.pin_d12 = 3;
            cfg.pin_d13 = 8;
            cfg.pin_d14 = 18;
            cfg.pin_d15 = 17;
            cfg.pin_henable = 40;
            cfg.pin_vsync = 7;
            cfg.pin_hsync = 15;
            cfg.pin_pclk = 41;
            cfg.hsync_front_porch = 10;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch = 20;
            cfg.vsync_front_porch = 10;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch = 20;
            cfg.hsync_polarity = true;
            cfg.vsync_polarity = true;
            cfg.pclk_active_neg = false;
            cfg.de_idle_high = false;
            cfg.pclk_idle_high = false;
            bus_.config(cfg);
        }

        panel_.setBus(&bus_);
        setPanel(&panel_);
    }

  private:
    lgfx::Bus_RGB bus_;
    CrowPanelRotary21Panel panel_;
};

struct ButtonState {
    bool lastRaw = true;
    bool stablePressed = false;
    uint32_t debounceStartedAt = 0;
    uint32_t stableChangedAt = 0;
    uint32_t pressObservedAt = 0;
    bool longLatched = false;
    bool pressObserved = false;
    bool idleLevel = true;
    bool debugHaveLastRead = false;
    bool debugLastReadOk = false;
    uint8_t debugLastExpander = 0xFF;
};

class CrowPanelRotary21Canvas : public RemoteCanvas {
  public:
    explicit CrowPanelRotary21Canvas(CrowPanelRotary21Display* display)
        : display_(display),
          sprite_(display) {}

    bool begin() {
        if(spriteReady_) {
            return true;
        }
        sprite_.setPsram(true);
        spriteReady_ = sprite_.createSprite(display_->width(), display_->height()) != nullptr;
        return spriteReady_;
    }

    uint16_t width() const override {
        return display_->width();
    }

    uint16_t height() const override {
        return display_->height();
    }

    void fillScreen(uint16_t color) override {
        if(spriteReady_) {
            sprite_.fillScreen(color);
            return;
        }
        display_->fillScreen(color);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        if(spriteReady_) {
            sprite_.fillRect(x, y, w, h, color);
            return;
        }
        display_->fillRect(x, y, w, h, color);
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        if(spriteReady_) {
            sprite_.drawRect(x, y, w, h, color);
            return;
        }
        display_->drawRect(x, y, w, h, color);
    }

    void fillRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        if(spriteReady_) {
            sprite_.fillRoundRect(x, y, w, h, radius, color);
            return;
        }
        display_->fillRoundRect(x, y, w, h, radius, color);
    }

    void drawRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        if(spriteReady_) {
            sprite_.drawRoundRect(x, y, w, h, radius, color);
            return;
        }
        display_->drawRoundRect(x, y, w, h, radius, color);
    }

    void fillCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        if(spriteReady_) {
            sprite_.fillCircle(x, y, radius, color);
            return;
        }
        display_->fillCircle(x, y, radius, color);
    }

    void drawCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        if(spriteReady_) {
            sprite_.drawCircle(x, y, radius, color);
            return;
        }
        display_->drawCircle(x, y, radius, color);
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
        if(spriteReady_) {
            sprite_.drawLine(x0, y0, x1, y1, color);
            return;
        }
        display_->drawLine(x0, y0, x1, y1, color);
    }

    void drawCenteredText(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        if(spriteReady_) {
            configureText(&sprite_, style, textColor, bgColor);
            sprite_.setTextDatum(middle_center);
            sprite_.drawString(text, x, y);
            return;
        }
        configureText(display_, style, textColor, bgColor);
        display_->setTextDatum(middle_center);
        display_->drawString(text, x, y);
    }

    void drawCenteredTextRotated(const String& text,
                                 int16_t x,
                                 int16_t y,
                                 float angleDegrees,
                                 RemoteTextStyle style,
                                 uint16_t textColor,
                                 uint16_t bgColor) override {
        if(text.length() == 0) {
            return;
        }

        configureText(display_, style, textColor, bgColor);
        const int16_t textW = max<int16_t>(1, static_cast<int16_t>(display_->textWidth(text)));
        const int16_t textH = max<int16_t>(1, static_cast<int16_t>(display_->fontHeight()));
        const int16_t spriteW = static_cast<int16_t>(textW + 10);
        const int16_t spriteH = static_cast<int16_t>(textH + 10);
        uint16_t transparent = 0x0001;
        if(transparent == textColor || transparent == bgColor) {
            transparent = 0xF81F;
        }

        LGFX_Sprite glyph(display_);
        glyph.setColorDepth(16);
        if(glyph.createSprite(spriteW, spriteH) == nullptr) {
            drawCenteredText(text, x, y, style, textColor, bgColor);
            return;
        }

        configureText(&glyph, style, textColor, transparent);
        glyph.fillSprite(transparent);
        glyph.setTextDatum(middle_center);
        glyph.drawString(text, spriteW / 2, spriteH / 2);
        glyph.setPivot(spriteW / 2.0f, spriteH / 2.0f);
        if(spriteReady_) {
            glyph.pushRotateZoomWithAA(&sprite_,
                                       static_cast<float>(x),
                                       static_cast<float>(y),
                                       angleDegrees,
                                       1.0f,
                                       1.0f,
                                       transparent);
        } else {
            glyph.pushRotateZoomWithAA(display_,
                                       static_cast<float>(x),
                                       static_cast<float>(y),
                                       angleDegrees,
                                       1.0f,
                                       1.0f,
                                       transparent);
        }
        glyph.deleteSprite();
    }

    void drawTextTopLeft(const String& text,
                         int16_t x,
                         int16_t y,
                         RemoteTextStyle style,
                         uint16_t textColor,
                         uint16_t bgColor) override {
        if(spriteReady_) {
            configureText(&sprite_, style, textColor, bgColor);
            sprite_.setTextDatum(top_left);
            sprite_.drawString(text, x, y);
            return;
        }
        configureText(display_, style, textColor, bgColor);
        display_->setTextDatum(top_left);
        display_->drawString(text, x, y);
    }

    void drawTextTopRight(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        if(spriteReady_) {
            configureText(&sprite_, style, textColor, bgColor);
            sprite_.setTextDatum(top_right);
            sprite_.drawString(text, x, y);
            return;
        }
        configureText(display_, style, textColor, bgColor);
        display_->setTextDatum(top_right);
        display_->drawString(text, x, y);
    }

    void present() override {
        if(spriteReady_) {
            sprite_.pushSprite(0, 0);
        }
    }

  private:
    uint8_t sizeForStyle(RemoteTextStyle style) const {
        switch(style) {
            case RemoteTextStyle::Tiny: return 1;
            case RemoteTextStyle::Small: return 2;
            case RemoteTextStyle::Medium: return 3;
            case RemoteTextStyle::Large: return 4;
            case RemoteTextStyle::Hero: return 5;
            default: return 1;
        }
    }

    template <typename TGfx>
    void configureText(TGfx* gfx, RemoteTextStyle style, uint16_t textColor, uint16_t bgColor) {
        gfx->setFont(nullptr);
        gfx->setTextSize(sizeForStyle(style));
        gfx->setTextColor(textColor, bgColor);
        gfx->setTextPadding(0);
    }

    CrowPanelRotary21Display* display_;
    LGFX_Sprite sprite_;
    bool spriteReady_ = false;
};

CrowPanelRotary21Display g_display;
CrowPanelRotary21Canvas g_canvas(&g_display);
Pcf8574Io g_expander;
CrowPanelRotary21Touch g_touch;
ESP32Encoder g_encoder;
ButtonState g_button;
RemoteTouchState g_touchState;
int32_t g_lastEncoderRaw = 0;
uint8_t g_expanderShadow = 0xFF;

const RemoteBoardInfo kBoardInfo = {
    RemoteBoardId::CrowPanelRotary21,
    "CrowPanel Rotary 2.1",
    kDisplayWidth,
    kDisplayHeight,
    true,
    true,
    static_cast<uint32_t>(RemoteBoardCapability::Touch) |
        static_cast<uint32_t>(RemoteBoardCapability::Encoder) |
        static_cast<uint32_t>(RemoteBoardCapability::Backlight) |
        static_cast<uint32_t>(RemoteBoardCapability::I2c) |
        static_cast<uint32_t>(RemoteBoardCapability::UsbConsole),
};

const RemoteBoardServices kBoardServices = {
    &Wire,
    nullptr,
    nullptr,
    &Serial,
    kBoardPins.touchSda,
    kBoardPins.touchScl,
    kBoardPins.displaySclk,
    kBoardPins.displayMosi,
    -1,
    kBoardPins.displayCs,
    -1,
    -1,
    nullptr,
    nullptr,
    nullptr,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    true,
};

const RemoteBoardFeatureMap kBoardFeatureMap = {
    {
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::None,
        0,
    },
    {},
    {},
    {},
    {},
    {},
    {
        RemoteBoardSupportLevel::Implemented,
        true,
    },
    {
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::None,
        true,
        true,
    },
};

bool writeExpander() {
    return g_expander.write(g_expanderShadow);
}

bool setExpanderBit(uint8_t bit, bool level) {
    if(level) {
        g_expanderShadow |= static_cast<uint8_t>(1u << bit);
    } else {
        g_expanderShadow &= static_cast<uint8_t>(~(1u << bit));
    }
    return writeExpander();
}

bool readExpanderBit(uint8_t bit, bool* level) {
    uint8_t value = 0xFF;
    if(!g_expander.read(&value)) {
        return false;
    }
    if(level != nullptr) {
        *level = (value & static_cast<uint8_t>(1u << bit)) != 0;
    }
    return true;
}

void logEncoderButtonRead(uint32_t now, bool readOk, uint8_t expanderValue, bool rawPressed) {
    if(!kDebugEncoderButton) {
        return;
    }
    if(g_button.debugHaveLastRead &&
       g_button.debugLastReadOk == readOk &&
       g_button.debugLastExpander == expanderValue) {
        return;
    }
    g_button.debugHaveLastRead = true;
    g_button.debugLastReadOk = readOk;
    g_button.debugLastExpander = expanderValue;
    Serial.printf("[remote][encbtn] t=%lu read=%d exp=0x%02X raw=%d stable=%d observed=%d long=%d idle=%d\n",
                  static_cast<unsigned long>(now),
                  readOk ? 1 : 0,
                  expanderValue,
                  rawPressed ? 1 : 0,
                  g_button.stablePressed ? 1 : 0,
                  g_button.pressObserved ? 1 : 0,
                  g_button.longLatched ? 1 : 0,
                  g_button.idleLevel ? 1 : 0);
}

void logEncoderButtonEvent(uint32_t now,
                           const char* eventName,
                           bool raw,
                           bool rawPressed,
                           uint8_t expanderValue) {
    if(!kDebugEncoderButton) {
        return;
    }
    Serial.printf("[remote][encbtn] t=%lu event=%s raw=%d rawPressed=%d stable=%d observed=%d long=%d exp=0x%02X obsAt=%lu stableAt=%lu\n",
                  static_cast<unsigned long>(now),
                  eventName,
                  raw ? 1 : 0,
                  rawPressed ? 1 : 0,
                  g_button.stablePressed ? 1 : 0,
                  g_button.pressObserved ? 1 : 0,
                  g_button.longLatched ? 1 : 0,
                  expanderValue,
                  static_cast<unsigned long>(g_button.pressObservedAt),
                  static_cast<unsigned long>(g_button.stableChangedAt));
}

bool pulseExpanderBit(uint8_t bit) {
    if(!setExpanderBit(bit, true)) {
        return false;
    }
    delay(100);
    if(!setExpanderBit(bit, false)) {
        return false;
    }
    delay(120);
    if(!setExpanderBit(bit, true)) {
        return false;
    }
    delay(120);
    return true;
}

bool prepareExpander() {
    g_expanderShadow = 0xFF;
    if(!writeExpander()) {
        return false;
    }

    if(!setExpanderBit(kExpanderDisplayPower, true)) {
        return false;
    }
    delay(100);

    if(!pulseExpanderBit(kExpanderDisplayReset)) {
        return false;
    }

    if(!pulseExpanderBit(kExpanderTouchReset)) {
        return false;
    }

    if(!setExpanderBit(kExpanderTouchInt, true)) {
        return false;
    }
    return true;
}

bool beginBoardI2c() {
    Wire.begin(kBoardPins.touchSda, kBoardPins.touchScl, kI2cClock);
    Wire.setTimeOut(20);
    return true;
}

String scanBoardI2cBus() {
    beginBoardI2c();
    String found;
    for(uint8_t address = 1; address < 0x78; ++address) {
        Wire.beginTransmission(address);
        if(Wire.endTransmission() == 0) {
            if(!found.isEmpty()) {
                found += ",";
            }
            char token[5];
            snprintf(token, sizeof(token), "0x%02X", address);
            found += token;
        }
    }
    if(found.isEmpty()) {
        found = "<none>";
    }
    return found;
}

bool beginExpander() {
    beginBoardI2c();
    if(!g_expander.begin(&Wire, kI2cAddressExpander)) {
        return false;
    }
    return prepareExpander();
}

bool waitForTouchReady(uint32_t timeoutMs, uint32_t intervalMs, uint32_t* elapsedMs = nullptr) {
    const uint32_t startedAt = millis();
    uint32_t elapsed = 0;
    while(true) {
        if(g_touch.ping()) {
            if(elapsedMs != nullptr) {
                *elapsedMs = elapsed;
            }
            return true;
        }

        elapsed = millis() - startedAt;
        if(elapsed >= timeoutMs) {
            if(elapsedMs != nullptr) {
                *elapsedMs = elapsed;
            }
            return false;
        }

        delay(intervalMs);
        elapsed = millis() - startedAt;
    }
}

bool readEncoderButtonRaw(bool* raw) {
    return readExpanderBit(kExpanderEncoderSwitch, raw);
}

void initBacklight() {
    ledcSetup(kBacklightPwmChannel, 5000, 8);
    ledcAttachPin(kBoardPins.backlight, kBacklightPwmChannel);
    ledcWrite(kBacklightPwmChannel, kBacklightDefault);
}

void initEncoderButton() {
    bool raw = true;
    readEncoderButtonRaw(&raw);
    g_button.idleLevel = raw;
    g_button.lastRaw = raw;
    g_button.debounceStartedAt = millis();
    g_button.stableChangedAt = g_button.debounceStartedAt;
    g_button.pressObservedAt = 0;
    g_button.stablePressed = false;
    g_button.longLatched = false;
    g_button.pressObserved = false;
    g_button.debugHaveLastRead = false;
    logEncoderButtonEvent(millis(), "init", raw, false, raw ? (1u << kExpanderEncoderSwitch) : 0);
}

void updateEncoderButton(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    const uint32_t now = millis();
    bool raw = g_button.lastRaw;
    uint8_t expanderValue = 0xFF;
    const bool readOk = g_expander.read(&expanderValue);
    if(readOk) {
        raw = (expanderValue & static_cast<uint8_t>(1u << kExpanderEncoderSwitch)) != 0;
    }
    const bool rawPressed = (raw != g_button.idleLevel);
    input->primaryPressed = rawPressed;
    logEncoderButtonRead(now, readOk, expanderValue, rawPressed);

    input->primaryShortPress = false;
    input->primaryLongPress = false;

    if(rawPressed) {
        if(!g_button.pressObserved) {
            g_button.pressObserved = true;
            g_button.pressObservedAt = now;
            logEncoderButtonEvent(now, "press-observed", raw, rawPressed, expanderValue);
        }
    }

    if(raw != g_button.lastRaw) {
        g_button.lastRaw = raw;
        g_button.debounceStartedAt = now;
        logEncoderButtonEvent(now, "raw-change", raw, rawPressed, expanderValue);
    }

    if(now - g_button.debounceStartedAt < kButtonDebounceMs) {
        return;
    }

    const bool pressed = input->primaryPressed;
    if(pressed != g_button.stablePressed) {
        g_button.stablePressed = pressed;
        g_button.stableChangedAt = now;
        if(pressed) {
            g_button.longLatched = false;
            logEncoderButtonEvent(now, "stable-press", raw, rawPressed, expanderValue);
        } else {
            if(!g_button.longLatched) {
                input->primaryShortPress = true;
                logEncoderButtonEvent(now, "short-press", raw, rawPressed, expanderValue);
            }
            g_button.pressObserved = false;
            g_button.pressObservedAt = 0;
            logEncoderButtonEvent(now, "stable-release", raw, rawPressed, expanderValue);
        }
    }

    if(rawPressed && g_button.pressObserved && !g_button.longLatched &&
       now - g_button.pressObservedAt >= kLongPressMs) {
        g_button.longLatched = true;
        input->primaryLongPress = true;
        logEncoderButtonEvent(now, "long-press", raw, rawPressed, expanderValue);
    }
}

int consumeEncoderDelta() {
    const int32_t raw = g_encoder.getCount();
    const int delta = static_cast<int>(g_lastEncoderRaw - raw);
    g_lastEncoderRaw = raw;
    return delta;
}

void updateTouch(RemoteInputState* input) {
    uint16_t x = 0;
    uint16_t y = 0;
    const bool touched = g_touch.readPoint(&x, &y);

    input->touch.justPressed = touched && !g_touchState.pressed;
    input->touch.justReleased = !touched && g_touchState.pressed;
    input->touch.pressed = touched;

    if(touched) {
        input->touch.x = static_cast<int16_t>(x);
        input->touch.y = static_cast<int16_t>(y);
    } else {
        input->touch.x = g_touchState.x;
        input->touch.y = g_touchState.y;
    }

    g_touchState = input->touch;
}

}  // namespace

bool crowpanel_rotary21_probe() {
    const bool expander = beginExpander();
    uint32_t touchReadyAfterMs = 0;
    const bool touch = expander && waitForTouchReady(kTouchProbeTimeoutMs, kTouchProbeIntervalMs, &touchReadyAfterMs);
    Serial.printf("[remote] crowpanel_rotary_21 bus 38/39 devices: %s\n", scanBoardI2cBus().c_str());
    Serial.printf("[remote] crowpanel_rotary_21 probe expander=%d touch=%d ready_after_ms=%lu\n",
                  expander,
                  touch,
                  static_cast<unsigned long>(touchReadyAfterMs));
    return touch;
}

bool crowpanel_rotary21_init() {
    if(!beginExpander()) {
        return false;
    }

    initBacklight();

    g_display.init();
    g_display.setRotation(0);
    g_display.fillScreen(kColorBlack);
    g_canvas.begin();

    uint32_t touchReadyAfterMs = 0;
    if(!waitForTouchReady(kTouchProbeTimeoutMs, kTouchProbeIntervalMs, &touchReadyAfterMs)) {
        Serial.printf("[remote] crowpanel_rotary_21 init touch timeout after %lu ms\n",
                      static_cast<unsigned long>(touchReadyAfterMs));
        return false;
    }
    Serial.printf("[remote] crowpanel_rotary_21 init touch ready after %lu ms\n",
                  static_cast<unsigned long>(touchReadyAfterMs));

    pinMode(kBoardPins.encoderA, INPUT_PULLUP);
    pinMode(kBoardPins.encoderB, INPUT_PULLUP);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    g_encoder.attachHalfQuad(kBoardPins.encoderA, kBoardPins.encoderB);
    g_encoder.setFilter(kEncoderFilterUs);
    g_encoder.setCount(0);
    delay(4);
    g_lastEncoderRaw = g_encoder.getCount();

    initEncoderButton();
    g_touchState = {};
    return true;
}

void crowpanel_rotary21_poll_input(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    *input = {};
    input->dialDelta = consumeEncoderDelta();
    updateEncoderButton(input);
    updateTouch(input);
}

RemoteCanvas& crowpanel_rotary21_canvas() {
    return g_canvas;
}

const RemoteBoardInfo& crowpanel_rotary21_info() {
    return kBoardInfo;
}

const RemoteBoardServices& crowpanel_rotary21_services() {
    return kBoardServices;
}

const RemoteBoardPins& crowpanel_rotary21_pins() {
    return kBoardPins;
}

const RemoteBoardFeatureMap& crowpanel_rotary21_feature_map() {
    return kBoardFeatureMap;
}

void crowpanel_rotary21_apply_indicator(const RemoteIndicatorState&) {}

bool crowpanel_rotary21_set_backlight(uint8_t level) {
    ledcWrite(kBacklightPwmChannel, level);
    return true;
}

RemoteBatteryStatus crowpanel_rotary21_battery_status() {
    return {};
}

int32_t crowpanel_rotary21_battery_millivolts() {
    return -1;
}

const RemoteBoardDriver kCrowPanelRotary21Driver = {
    "crowpanel_rotary_21",
    crowpanel_rotary21_probe,
    crowpanel_rotary21_init,
    crowpanel_rotary21_poll_input,
    crowpanel_rotary21_canvas,
    crowpanel_rotary21_info,
    crowpanel_rotary21_services,
    crowpanel_rotary21_pins,
    crowpanel_rotary21_feature_map,
    crowpanel_rotary21_apply_indicator,
    crowpanel_rotary21_set_backlight,
    crowpanel_rotary21_battery_status,
    crowpanel_rotary21_battery_millivolts,
};

const RemoteBoardDriver* remote_board_crowpanel_rotary21_driver() {
    return &kCrowPanelRotary21Driver;
}
