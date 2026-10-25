#include "remote_board_driver.h"

#include <Adafruit_NeoPixel.h>
#include <ESP32Encoder.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

namespace {

constexpr uint8_t kDisplayWidth = 240;
constexpr uint8_t kDisplayHeight = 240;

constexpr uint8_t kLedRingCount = 5;
constexpr uint8_t kOledAddressPrimary = 0x3C;
constexpr uint8_t kOledAddressSecondary = 0x3D;
constexpr uint8_t kAuxPowerPin1 = 1;
constexpr uint8_t kAuxPowerPin2 = 2;

constexpr uint32_t kButtonDebounceMs = 18;
constexpr uint32_t kLongPressMs = 750;
constexpr uint32_t kTouchRetryMs = 1000;
constexpr uint16_t kColorBlack = 0x0000;

const RemoteBoardPins kBoardPins = [] {
    RemoteBoardPins pins = {};
    pins.touchSda = 6;
    pins.touchScl = 7;
    pins.touchInt = 5;
    pins.touchReset = 13;
    pins.displaySclk = 10;
    pins.displayMosi = 11;
    pins.displayDc = 3;
    pins.displayCs = 9;
    pins.displayReset = 14;
    pins.backlight = 46;
    pins.powerEnable = 40;
    pins.encoderA = 45;
    pins.encoderB = 42;
    pins.encoderSwitch = 41;
    pins.primaryButton = 41;
    pins.indicatorData = 48;
    pins.expansionI2cSda = 38;
    pins.expansionI2cScl = 39;
    pins.expansionGpio0 = 4;
    pins.expansionGpio1 = 12;
    return pins;
}();

class CrowPanelDisplay : public lgfx::LGFX_Device {
  public:
    CrowPanelDisplay() {
        {
            auto cfg = bus_.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 80000000;
            cfg.freq_read = 20000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = kBoardPins.displaySclk;
            cfg.pin_mosi = kBoardPins.displayMosi;
            cfg.pin_miso = kBoardPins.displayMiso;
            cfg.pin_dc = kBoardPins.displayDc;
            bus_.config(cfg);
            panel_.setBus(&bus_);
        }
        {
            auto cfg = panel_.config();
            cfg.pin_cs = kBoardPins.displayCs;
            cfg.pin_rst = kBoardPins.displayReset;
            cfg.pin_busy = -1;
            cfg.memory_width = kDisplayWidth;
            cfg.memory_height = kDisplayHeight;
            cfg.panel_width = kDisplayWidth;
            cfg.panel_height = kDisplayHeight;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            panel_.config(cfg);
        }
        setPanel(&panel_);
    }

  private:
    lgfx::Panel_GC9A01 panel_;
    lgfx::Bus_SPI bus_;
};

class Cst816Touch {
  public:
    bool probe() {
        return begin();
    }

    bool begin() {
        enablePeripheralPower();
        wire_ = nullptr;

        const TouchBusCandidate candidates[] = {
            {&Wire1, kBoardPins.touchSda, kBoardPins.touchScl, "touch"},
            {&Wire, kBoardPins.expansionI2cSda, kBoardPins.expansionI2cScl, "aux"},
        };

        for(const TouchBusCandidate& candidate : candidates) {
            wire_ = candidate.wire;
            configureBus(candidate);
            resetController();
            pinMode(kBoardPins.touchInt, INPUT_PULLUP);
            if(!pingAddress()) {
                continue;
            }
            if(!writeRegister(0xFE, 0xFF)) {
                continue;
            }

            wire_ = candidate.wire;
            remote_board_console().printf("[remote] crowpanel touch bus: %s (%d/%d)\n",
                                          candidate.name,
                                          candidate.sda,
                                          candidate.scl);
            return true;
        }

        return false;
    }

    bool readPoint(uint16_t* x, uint16_t* y) {
        if(wire_ == nullptr) {
            return false;
        }

        uint8_t touchPoints = 0;
        if(!readRegister(0x02, &touchPoints) || touchPoints == 0) {
            return false;
        }

        uint8_t data[4] = {};
        if(!readRegisters(0x03, data, sizeof(data))) {
            return false;
        }

        const uint16_t rawX = static_cast<uint16_t>(((data[0] & 0x0F) << 8) | data[1]);
        const uint16_t rawY = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
        if(rawX >= kDisplayWidth || rawY >= kDisplayHeight) {
            return false;
        }

        if(x != nullptr) *x = rawX;
        if(y != nullptr) *y = rawY;
        return true;
    }

  private:
    static constexpr uint8_t kAddress = 0x15;

    struct TouchBusCandidate {
        TwoWire* wire;
        int8_t sda;
        int8_t scl;
        const char* name;
    };

    void enablePeripheralPower() {
        pinMode(kBoardPins.powerEnable, OUTPUT);
        digitalWrite(kBoardPins.powerEnable, LOW);
        pinMode(kAuxPowerPin1, OUTPUT);
        digitalWrite(kAuxPowerPin1, HIGH);
        pinMode(kAuxPowerPin2, OUTPUT);
        digitalWrite(kAuxPowerPin2, HIGH);
        delay(20);
    }

    void resetController() {
        pinMode(kBoardPins.touchInt, OUTPUT);
        digitalWrite(kBoardPins.touchInt, HIGH);
        delay(1);
        digitalWrite(kBoardPins.touchInt, LOW);
        delay(1);
        pinMode(kBoardPins.touchReset, OUTPUT);
        digitalWrite(kBoardPins.touchReset, LOW);
        delay(10);
        digitalWrite(kBoardPins.touchReset, HIGH);
        delay(300);
    }

    void configureBus(const TouchBusCandidate& candidate) {
        candidate.wire->begin(candidate.sda, candidate.scl, 400000U);
        candidate.wire->setTimeOut(20);
    }

    bool pingAddress() {
        if(wire_ == nullptr) {
            return false;
        }
        wire_->beginTransmission(kAddress);
        return wire_->endTransmission() == 0;
    }

    bool readRegister(uint8_t reg, uint8_t* value) {
        if(value == nullptr) {
            return false;
        }
        return readRegisters(reg, value, 1);
    }

    bool readRegisters(uint8_t reg, uint8_t* data, size_t len) {
        if(wire_ == nullptr) {
            return false;
        }

        wire_->beginTransmission(kAddress);
        wire_->write(reg);
        if(wire_->endTransmission(true) != 0) {
            return false;
        }

        const size_t received = wire_->requestFrom(static_cast<uint16_t>(kAddress), len, true);
        if(received != len) {
            while(wire_->available()) {
                wire_->read();
            }
            return false;
        }

        for(size_t i = 0; i < len; ++i) {
            data[i] = static_cast<uint8_t>(wire_->read());
        }
        return true;
    }

    bool writeRegister(uint8_t reg, uint8_t value) {
        if(wire_ == nullptr) {
            return false;
        }

        wire_->beginTransmission(kAddress);
        wire_->write(reg);
        wire_->write(value);
        return wire_->endTransmission() == 0;
    }

    TwoWire* wire_ = nullptr;
};

struct ButtonState {
    bool lastRaw = true;
    bool stablePressed = false;
    bool releaseEvent = false;
    bool longEvent = false;
    bool longLatched = false;
    uint32_t debounceStartedAt = 0;
    uint32_t stableChangedAt = 0;
    bool idleLevel = true;
};

class CrowPanelCanvas : public RemoteCanvas {
  public:
    explicit CrowPanelCanvas(CrowPanelDisplay* display) : display_(display) {}

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

    void drawRgb565Bitmap(int16_t x, int16_t y, const uint16_t* pixels, int16_t w, int16_t h) override {
        if(pixels != nullptr) display_->pushImage(x, y, w, h, pixels);
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
            case RemoteTextStyle::Small: return 1;
            case RemoteTextStyle::Medium: return 2;
            case RemoteTextStyle::Large: return 3;
            case RemoteTextStyle::Hero: return 4;
            default: return 1;
        }
    }

    void configureText(RemoteTextStyle style, uint16_t textColor, uint16_t bgColor) {
        display_->setFont(nullptr);
        display_->setTextSize(sizeForStyle(style));
        display_->setTextColor(textColor, bgColor);
        display_->setTextPadding(0);
    }

    CrowPanelDisplay* display_;
};

CrowPanelDisplay g_display;
CrowPanelCanvas g_canvas(&g_display);
Cst816Touch g_touch;
ESP32Encoder g_encoder;
Adafruit_NeoPixel g_ring(kLedRingCount, kBoardPins.indicatorData, NEO_GRB + NEO_KHZ800);
ButtonState g_button;
RemoteTouchState g_touchState;
RemoteIndicatorState g_lastIndicator;
bool g_indicatorValid = false;
bool g_touchAvailable = false;
int32_t g_lastEncoderRaw = 0;
uint32_t g_nextTouchRetryAt = 0;

const RemoteBoardInfo kBoardInfo = {
    RemoteBoardId::CrowPanelRotary,
    "CrowPanel Rotary 1.28",
    kDisplayWidth,
    kDisplayHeight,
    true,
    true,
    static_cast<uint32_t>(RemoteBoardCapability::Touch) |
        static_cast<uint32_t>(RemoteBoardCapability::Encoder) |
    static_cast<uint32_t>(RemoteBoardCapability::Indicator) |
        static_cast<uint32_t>(RemoteBoardCapability::Backlight) |
        static_cast<uint32_t>(RemoteBoardCapability::I2c) |
        static_cast<uint32_t>(RemoteBoardCapability::DeepSleep) |
        static_cast<uint32_t>(RemoteBoardCapability::WakeSource) |
        static_cast<uint32_t>(RemoteBoardCapability::UsbConsole) |
        static_cast<uint32_t>(RemoteBoardCapability::ExpansionGpio) |
        static_cast<uint32_t>(RemoteBoardCapability::ExpansionI2c),
};

const RemoteBoardServices kBoardServices = {
    &Wire1,
    nullptr,
    nullptr,
    &Serial,
    kBoardPins.touchSda,
    kBoardPins.touchScl,
    kBoardPins.displaySclk,
    kBoardPins.displayMosi,
    kBoardPins.displayMiso,
    kBoardPins.displayCs,
    -1,
    -1,
    &Wire,
    nullptr,
    nullptr,
    kBoardPins.expansionI2cSda,
    kBoardPins.expansionI2cScl,
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
        RemoteBoardSupportLevel::Present,
        static_cast<uint32_t>(RemoteWakeSource::Touch) |
            static_cast<uint32_t>(RemoteWakeSource::PrimaryButton) |
            static_cast<uint32_t>(RemoteWakeSource::EncoderSwitch),
    },
    {},
    {},
    {},
    {},
    {
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::Present,
        2,
    },
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

void initBacklight() {
    pinMode(kBoardPins.powerEnable, OUTPUT);
    digitalWrite(kBoardPins.powerEnable, LOW);
    pinMode(kAuxPowerPin1, OUTPUT);
    digitalWrite(kAuxPowerPin1, HIGH);
    pinMode(kAuxPowerPin2, OUTPUT);
    digitalWrite(kAuxPowerPin2, HIGH);
    ledcAttach(kBoardPins.backlight, 5000, 8);
    ledcWrite(kBoardPins.backlight, 255);
}

void initEncoderButton() {
    pinMode(kBoardPins.encoderSwitch, INPUT);
    g_button.idleLevel = digitalRead(kBoardPins.encoderSwitch);
    g_button.lastRaw = g_button.idleLevel;
    g_button.debounceStartedAt = millis();
    g_button.stableChangedAt = g_button.debounceStartedAt;
}

void updateEncoderButton(RemoteInputState* input) {
    const uint32_t now = millis();
    const bool raw = digitalRead(kBoardPins.encoderSwitch);
    input->primaryPressed = (raw != g_button.idleLevel);
    input->primaryShortPress = false;
    input->primaryLongPress = false;

    if(raw != g_button.lastRaw) {
        g_button.lastRaw = raw;
        g_button.debounceStartedAt = now;
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

bool probeExpansionDevice(uint8_t address) {
    Wire.begin(kBoardPins.expansionI2cSda, kBoardPins.expansionI2cScl, 400000U);
    Wire.setTimeOut(20);
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

bool probeOnboardOled() {
    return probeExpansionDevice(kOledAddressPrimary) || probeExpansionDevice(kOledAddressSecondary);
}

bool tryBeginTouch(bool logFailure) {
    if(g_touch.begin()) {
        if(!g_touchAvailable) {
            remote_board_console().println("[remote] crowpanel touch online");
        }
        g_touchAvailable = true;
        g_nextTouchRetryAt = 0;
        return true;
    }

    g_touchAvailable = false;
    g_nextTouchRetryAt = millis() + kTouchRetryMs;
    if(logFailure) {
        remote_board_console().println("[remote] crowpanel touch unavailable, continuing without touch");
    }
    return false;
}

void updateTouch(RemoteInputState* input) {
    if(!g_touchAvailable) {
        if(millis() >= g_nextTouchRetryAt) {
            tryBeginTouch(false);
        }
        return;
    }

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

uint32_t indicatorColor(const RemoteIndicatorState& indicator) {
    if(!indicator.linkOnline) {
        return g_ring.Color(70, 0, 0);
    }
    if(indicator.hasError) {
        return g_ring.Color(90, 30, 0);
    }
    if(indicator.running) {
        return g_ring.Color(0, 70, 15);
    }
    if(indicator.gbwMode) {
        return g_ring.Color(80, 45, 0);
    }
    return g_ring.Color(0, 30, 90);
}

}  // namespace

bool crowpanel_probe() {
    const bool touch = g_touch.probe();
    const bool oled = probeOnboardOled();
    return touch || oled;
}

bool crowpanel_init() {
    Wire.begin(kBoardPins.expansionI2cSda, kBoardPins.expansionI2cScl, 400000U);
    Wire.setTimeOut(20);

    initBacklight();

    g_display.init();
    g_display.initDMA();
    g_display.startWrite();
    g_display.setRotation(0);
    g_display.fillScreen(kColorBlack);

    tryBeginTouch(true);

    pinMode(kBoardPins.powerEnable, OUTPUT);
    digitalWrite(kBoardPins.powerEnable, LOW);

    g_ring.begin();
    g_ring.clear();
    g_ring.show();

    initEncoderButton();
    pinMode(kBoardPins.encoderA, INPUT);
    pinMode(kBoardPins.encoderB, INPUT);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    g_encoder.attachHalfQuad(kBoardPins.encoderA, kBoardPins.encoderB);
    g_encoder.setFilter(100);
    g_encoder.setCount(0);
    g_lastEncoderRaw = 0;
    g_touchState = {};
    g_touchAvailable = false;
    g_nextTouchRetryAt = 0;
    tryBeginTouch(false);
    g_indicatorValid = false;
    return true;
}

void crowpanel_poll_input(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    *input = {};
    input->dialDelta = consumeEncoderDelta();
    updateEncoderButton(input);
    updateTouch(input);
}

RemoteCanvas& crowpanel_canvas() {
    return g_canvas;
}

const RemoteBoardInfo& crowpanel_info() {
    return kBoardInfo;
}

const RemoteBoardServices& crowpanel_services() {
    return kBoardServices;
}

const RemoteBoardPins& crowpanel_pins() {
    return kBoardPins;
}

const RemoteBoardFeatureMap& crowpanel_feature_map() {
    return kBoardFeatureMap;
}

void crowpanel_apply_indicator(const RemoteIndicatorState& indicator) {
    if(g_indicatorValid &&
       g_lastIndicator.linkOnline == indicator.linkOnline &&
       g_lastIndicator.running == indicator.running &&
       g_lastIndicator.gbwMode == indicator.gbwMode &&
       g_lastIndicator.hasError == indicator.hasError) {
        if(!indicator.linkOnline && ((millis() / 250) % 2 == 1)) {
            for(uint8_t i = 0; i < kLedRingCount; ++i) {
                g_ring.setPixelColor(i, 0);
            }
            g_ring.show();
        }
        return;
    }

    g_lastIndicator = indicator;
    g_indicatorValid = true;

    const bool blinkOff = !indicator.linkOnline && ((millis() / 250) % 2 == 1);
    const uint32_t color = indicatorColor(indicator);
    for(uint8_t i = 0; i < kLedRingCount; ++i) {
        g_ring.setPixelColor(i, blinkOff ? 0 : color);
    }
    g_ring.show();
}

bool crowpanel_set_backlight(uint8_t level) {
    ledcWrite(kBoardPins.backlight, level);
    return true;
}

RemoteBatteryStatus crowpanel_battery_status() {
    return {};
}

int32_t crowpanel_battery_millivolts() {
    return -1;
}

bool crowpanel_input_pending() {
    return false;
}

const RemoteBoardDriver kCrowPanelDriver = {
    "crowpanel_rotary_128",
    crowpanel_probe,
    crowpanel_init,
    crowpanel_poll_input,
    crowpanel_canvas,
    crowpanel_info,
    crowpanel_services,
    crowpanel_pins,
    crowpanel_feature_map,
    crowpanel_apply_indicator,
    crowpanel_set_backlight,
    crowpanel_battery_status,
    crowpanel_battery_millivolts,
    crowpanel_input_pending,
};

const RemoteBoardDriver* remote_board_crowpanel_driver() {
    return &kCrowPanelDriver;
}
