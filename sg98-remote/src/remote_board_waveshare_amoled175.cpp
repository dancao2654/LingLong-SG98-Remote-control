#include "remote_board_driver.h"

#include <Arduino_GFX_Library.h>
#include <TouchDrv.hpp>
#include <Wire.h>
#include <XPowersLib.h>

namespace {

constexpr int kDisplayWidth = 466;
constexpr int kDisplayHeight = 466;

const RemoteBoardPins kBoardPins = [] {
    RemoteBoardPins pins = {};
    pins.displayData0 = 4;
    pins.displayData1 = 5;
    pins.displayData2 = 6;
    pins.displayData3 = 7;
    pins.displaySclk = 38;
    pins.displayCs = 12;
    pins.displayReset = 39;
    pins.touchSda = 15;
    pins.touchScl = 14;
    pins.touchInt = 11;
    pins.touchReset = 40;
    return pins;
}();

class WaveshareCanvas : public RemoteCanvas {
  public:
    explicit WaveshareCanvas(Arduino_GFX* display) : display_(display) {}

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
        int16_t x1 = 0;
        int16_t y1 = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        display_->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        display_->setCursor(x - static_cast<int16_t>(w / 2), y - static_cast<int16_t>(h / 2));
        display_->print(text);
    }

    void drawTextTopLeft(const String& text,
                         int16_t x,
                         int16_t y,
                         RemoteTextStyle style,
                         uint16_t textColor,
                         uint16_t bgColor) override {
        configureText(style, textColor, bgColor);
        display_->setCursor(x, y + baselineOffset(style));
        display_->print(text);
    }

    void drawTextTopRight(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        configureText(style, textColor, bgColor);
        int16_t x1 = 0;
        int16_t y1 = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        display_->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        display_->setCursor(x - static_cast<int16_t>(w), y + baselineOffset(style));
        display_->print(text);
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

    int16_t baselineOffset(RemoteTextStyle style) const {
        return static_cast<int16_t>(8 * sizeForStyle(style));
    }

    void configureText(RemoteTextStyle style, uint16_t textColor, uint16_t bgColor) {
        display_->setFont(nullptr);
        display_->setTextSize(sizeForStyle(style));
        display_->setTextWrap(false);
        display_->setTextColor(textColor, bgColor);
    }

    Arduino_GFX* display_;
};

Arduino_DataBus* g_bus = new Arduino_ESP32QSPI(
    kBoardPins.displayCs,
    kBoardPins.displaySclk,
    kBoardPins.displayData0,
    kBoardPins.displayData1,
    kBoardPins.displayData2,
    kBoardPins.displayData3);

Arduino_CO5300* g_display = new Arduino_CO5300(
    g_bus,
    kBoardPins.displayReset,
    0,
    kDisplayWidth,
    kDisplayHeight,
    6,
    0,
    0,
    0);

WaveshareCanvas g_canvas(g_display);
TouchDrvCST92xx g_touch;
XPowersAXP2101 g_pmic;
RemoteTouchState g_touchState;
bool g_pmicReady = false;

const RemoteBoardInfo kBoardInfo = {
    RemoteBoardId::WaveshareAmoled175,
    "Waveshare AMOLED 1.75",
    kDisplayWidth,
    kDisplayHeight,
    true,
    false,
    static_cast<uint32_t>(RemoteBoardCapability::Touch) |
        static_cast<uint32_t>(RemoteBoardCapability::Backlight) |
        static_cast<uint32_t>(RemoteBoardCapability::I2c) |
        static_cast<uint32_t>(RemoteBoardCapability::BatterySense) |
        static_cast<uint32_t>(RemoteBoardCapability::AudioInput) |
        static_cast<uint32_t>(RemoteBoardCapability::AudioOutput) |
        static_cast<uint32_t>(RemoteBoardCapability::MotionSensor) |
        static_cast<uint32_t>(RemoteBoardCapability::Rtc) |
        static_cast<uint32_t>(RemoteBoardCapability::Pmic) |
        static_cast<uint32_t>(RemoteBoardCapability::DeepSleep) |
        static_cast<uint32_t>(RemoteBoardCapability::WakeSource) |
        static_cast<uint32_t>(RemoteBoardCapability::ExpansionGpio) |
        static_cast<uint32_t>(RemoteBoardCapability::ExpansionUart) |
        static_cast<uint32_t>(RemoteBoardCapability::UsbDetect) |
        static_cast<uint32_t>(RemoteBoardCapability::ChargeState) |
        static_cast<uint32_t>(RemoteBoardCapability::UsbConsole),
};

const RemoteBoardServices kBoardServices = {
    &Wire,
    nullptr,
    nullptr,
    &Serial,
    kBoardPins.touchSda,
    kBoardPins.touchScl,
    -1,
    -1,
    -1,
    -1,
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
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Present,
        static_cast<uint32_t>(RemoteWakeSource::Touch),
    },
    {
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
    },
    {
        RemoteBoardSupportLevel::Present,
        RemoteBoardSupportLevel::Present,
        true,
        true,
    },
    {
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Mapped,
    },
    {},
    {
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::Present,
        3,
    },
    {
        RemoteBoardSupportLevel::Implemented,
        true,
    },
    {
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::None,
        true,
        true,
    },
};

bool pingTouchController() {
    pinMode(kBoardPins.touchReset, OUTPUT);
    digitalWrite(kBoardPins.touchReset, LOW);
    delay(30);
    digitalWrite(kBoardPins.touchReset, HIGH);
    delay(50);
    Wire.begin(kBoardPins.touchSda, kBoardPins.touchScl, 400000U);
    Wire.beginTransmission(0x5A);
    return Wire.endTransmission() == 0;
}

}  // namespace

bool waveshare_probe() {
    return pingTouchController();
}

bool waveshare_init() {
    Wire.begin(kBoardPins.touchSda, kBoardPins.touchScl, 400000U);

    pinMode(kBoardPins.touchReset, OUTPUT);
    digitalWrite(kBoardPins.touchReset, LOW);
    delay(30);
    digitalWrite(kBoardPins.touchReset, HIGH);
    delay(50);

    g_touch.setPins(kBoardPins.touchReset, kBoardPins.touchInt);
    if(!g_touch.begin(Wire, 0x5A, kBoardPins.touchSda, kBoardPins.touchScl)) {
        return false;
    }
    g_touch.reset();
    g_touch.setMaxCoordinates(kDisplayWidth, kDisplayHeight);
    g_touch.setMirrorXY(true, true);

    g_pmicReady = g_pmic.begin(Wire, AXP2101_SLAVE_ADDRESS, kBoardPins.touchSda, kBoardPins.touchScl);
    if(g_pmicReady) {
        g_pmic.enableBattDetection();
        g_pmic.enableBattVoltageMeasure();
        g_pmic.enableVbusVoltageMeasure();
        g_pmic.enableSystemVoltageMeasure();
    } else {
        Serial.println("[remote] waveshare pmic init failed");
    }

    if(!g_display->begin()) {
        return false;
    }
    g_display->setRotation(0);
    g_display->setBrightness(200);
    g_display->fillScreen(0x0000);
    g_touchState = {};
    return true;
}

void waveshare_poll_input(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    *input = {};

    const TouchPoints& points = g_touch.getTouchPoints();
    const bool touched = points.hasPoints();

    input->touch.justPressed = touched && !g_touchState.pressed;
    input->touch.justReleased = !touched && g_touchState.pressed;
    input->touch.pressed = touched;

    if(touched) {
        input->touch.x = static_cast<int16_t>(points[0].x);
        input->touch.y = static_cast<int16_t>(points[0].y);
    } else {
        input->touch.x = g_touchState.x;
        input->touch.y = g_touchState.y;
    }

    g_touchState = input->touch;
}

RemoteCanvas& waveshare_canvas() {
    return g_canvas;
}

const RemoteBoardInfo& waveshare_info() {
    return kBoardInfo;
}

const RemoteBoardServices& waveshare_services() {
    return kBoardServices;
}

const RemoteBoardPins& waveshare_pins() {
    return kBoardPins;
}

const RemoteBoardFeatureMap& waveshare_feature_map() {
    return kBoardFeatureMap;
}

void waveshare_apply_indicator(const RemoteIndicatorState&) {}

bool waveshare_set_backlight(uint8_t level) {
    g_display->setBrightness(level);
    return true;
}

RemoteBatteryStatus waveshare_battery_status() {
    RemoteBatteryStatus status = {};
    status.supported = g_pmicReady;
    if(!g_pmicReady) {
        return status;
    }

    status.batteryPresent = g_pmic.isBatteryConnect();
    status.usbPresent = g_pmic.isVbusIn();
    status.charging = g_pmic.isCharging();
    status.discharging = g_pmic.isDischarge();
    status.standby = g_pmic.isStandby();
    status.systemMillivolts = static_cast<int32_t>(g_pmic.getSystemVoltage());
    status.vbusMillivolts = status.usbPresent ? static_cast<int32_t>(g_pmic.getVbusVoltage()) : -1;
    status.millivolts = status.batteryPresent ? static_cast<int32_t>(g_pmic.getBattVoltage()) : -1;

    const int percent = g_pmic.getBatteryPercent();
    if(percent >= 0 && percent <= 100) {
        status.percent = static_cast<int8_t>(percent);
    }

    return status;
}

int32_t waveshare_battery_millivolts() {
    return waveshare_battery_status().millivolts;
}

const RemoteBoardDriver kWaveshareDriver = {
    "waveshare_amoled_175",
    waveshare_probe,
    waveshare_init,
    waveshare_poll_input,
    waveshare_canvas,
    waveshare_info,
    waveshare_services,
    waveshare_pins,
    waveshare_feature_map,
    waveshare_apply_indicator,
    waveshare_set_backlight,
    waveshare_battery_status,
    waveshare_battery_millivolts,
};

const RemoteBoardDriver* remote_board_waveshare_amoled175_driver() {
    return &kWaveshareDriver;
}
