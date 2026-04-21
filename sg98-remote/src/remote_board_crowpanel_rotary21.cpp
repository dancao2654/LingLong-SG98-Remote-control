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

constexpr uint32_t kI2cClock = 400000U;
constexpr uint8_t kI2cAddressExpander = 0x21;
constexpr uint8_t kI2cAddressTouch = 0x15;

constexpr uint8_t kExpanderTouchReset = 0;
constexpr uint8_t kExpanderTouchInt = 2;
constexpr uint8_t kExpanderDisplayPower = 3;
constexpr uint8_t kExpanderDisplayReset = 4;
constexpr uint8_t kExpanderEncoderSwitch = 5;

constexpr uint32_t kButtonDebounceMs = 18;
constexpr uint32_t kLongPressMs = 750;

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
    bool longLatched = false;
    bool idleLevel = true;
};

class CrowPanelRotary21Canvas : public RemoteCanvas {
  public:
    explicit CrowPanelRotary21Canvas(CrowPanelRotary21Display* display) : display_(display) {}

    uint16_t width() const override {
        return display_->width();
    }

    uint16_t height() const override {
        return display_->height();
    }

    void fillScreen(uint16_t color) override {
        display_->fillScreen(color);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        display_->fillRect(x, y, w, h, color);
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        display_->drawRect(x, y, w, h, color);
    }

    void fillRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        display_->fillRoundRect(x, y, w, h, radius, color);
    }

    void drawRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        display_->drawRoundRect(x, y, w, h, radius, color);
    }

    void fillCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        display_->fillCircle(x, y, radius, color);
    }

    void drawCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        display_->drawCircle(x, y, radius, color);
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
        display_->drawLine(x0, y0, x1, y1, color);
    }

    void drawCenteredText(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        configureText(style, textColor, bgColor);
        display_->setTextDatum(middle_center);
        display_->drawString(text, x, y);
    }

    void drawTextTopLeft(const String& text,
                         int16_t x,
                         int16_t y,
                         RemoteTextStyle style,
                         uint16_t textColor,
                         uint16_t bgColor) override {
        configureText(style, textColor, bgColor);
        display_->setTextDatum(top_left);
        display_->drawString(text, x, y);
    }

    void drawTextTopRight(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        configureText(style, textColor, bgColor);
        display_->setTextDatum(top_right);
        display_->drawString(text, x, y);
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

    void configureText(RemoteTextStyle style, uint16_t textColor, uint16_t bgColor) {
        display_->setFont(nullptr);
        display_->setTextSize(sizeForStyle(style));
        display_->setTextColor(textColor, bgColor);
        display_->setTextPadding(0);
    }

    CrowPanelRotary21Display* display_;
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
    return true;
}

bool beginExpander() {
    beginBoardI2c();
    if(!g_expander.begin(&Wire, kI2cAddressExpander)) {
        return false;
    }
    return prepareExpander();
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
    g_button.stablePressed = false;
    g_button.longLatched = false;
}

void updateEncoderButton(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    const uint32_t now = millis();
    bool raw = g_button.lastRaw;
    readEncoderButtonRaw(&raw);

    input->primaryShortPress = false;
    input->primaryLongPress = false;

    if(raw != g_button.lastRaw) {
        g_button.lastRaw = raw;
        g_button.debounceStartedAt = now;
    }

    if(now - g_button.debounceStartedAt < kButtonDebounceMs) {
        return;
    }

    const bool pressed = (raw != g_button.idleLevel);
    if(pressed != g_button.stablePressed) {
        g_button.stablePressed = pressed;
        g_button.stableChangedAt = now;
        if(pressed) {
            g_button.longLatched = false;
        } else if(!g_button.longLatched) {
            input->primaryShortPress = true;
        }
    }

    if(g_button.stablePressed && !g_button.longLatched &&
       now - g_button.stableChangedAt >= kLongPressMs) {
        g_button.longLatched = true;
        input->primaryLongPress = true;
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
    if(!beginExpander()) {
        return false;
    }
    return g_touch.ping();
}

bool crowpanel_rotary21_init() {
    if(!beginExpander()) {
        return false;
    }

    initBacklight();

    g_display.init();
    g_display.setRotation(0);
    g_display.fillScreen(kColorBlack);

    if(!g_touch.ping()) {
        return false;
    }

    pinMode(kBoardPins.encoderA, INPUT);
    pinMode(kBoardPins.encoderB, INPUT);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    g_encoder.attachHalfQuad(kBoardPins.encoderA, kBoardPins.encoderB);
    g_encoder.setFilter(100);
    g_encoder.setCount(0);
    g_lastEncoderRaw = 0;

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
