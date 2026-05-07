#include "remote_board_driver.h"
#include "remote_config.h"
#include "remote_runtime.h"

#if defined(REMOTE_ENABLE_LILYGO_T4_S3_H678) || defined(REMOTE_BOARD_FORCE_LILYGO_T4_S3_H678)

#include <Adafruit_GFX.h>
#include <Arduino_GFX_Library.h>
#include <SensorQMI8658.hpp>
#include <SPI.h>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>

namespace {

constexpr uint16_t kPanelNativeWidth = RM690B0_TFTWIDTH;
constexpr uint16_t kPanelNativeHeight = RM690B0_TFTHEIGHT;
constexpr uint16_t kDisplayWidth = kPanelNativeHeight;
constexpr uint16_t kDisplayHeight = kPanelNativeWidth;
constexpr uint8_t kDisplayRotation = 1;
constexpr uint8_t kTouchAddress = 0x5A;
constexpr uint8_t kPmicAddress = 0x6A;
constexpr uint32_t kI2cClock = 100000U;
constexpr uint32_t kButtonDebounceMs = 18;
constexpr uint32_t kLongPressMs = 750;
constexpr uint32_t kButtonPollMs = 10;
constexpr uint32_t kTouchPollMs = 8;
constexpr uint32_t kTouchIdlePollMs = 8;
constexpr uint32_t kTouchReleaseHoldMs = 55;
constexpr uint32_t kPmicStatusPollMs = remote_config::kBatteryStatusPollMs;
constexpr int8_t kBatteryPercentJitter = 1;
constexpr int8_t kBatteryTopOffPercent = 98;
constexpr int32_t kBatteryVoltageJitterMv = 25;
constexpr int32_t kBatteryVbusJitterMv = 150;
constexpr int32_t kBatteryTopOffMv = 4180;
constexpr uint8_t kImuDefaultAddress = QMI8658_L_SLAVE_ADDRESS;
constexpr SensorQMI8658::AccelODR kImuWakeOdr = SensorQMI8658::ACC_ODR_LOWPOWER_11Hz;
constexpr SensorQMI8658::AccelRange kImuAccelRange = SensorQMI8658::ACC_RANGE_8G;
constexpr float kImuWakeOdrLabelHz = 11.0f;
constexpr float kImuAccelScaleG = 8.0f / 32768.0f;
constexpr float kDisplayAutoRotateDominantG = 0.62f;
constexpr float kDisplayAutoRotateAxisMarginG = 0.22f;
constexpr float kDisplayAutoRotateFlatRejectMarginG = 0.12f;
constexpr uint8_t kDisplayAutoRotateConfirmSamples = 2;
constexpr uint8_t kDisplayAutoRotateCorrection = 0;
constexpr bool kDisplayAutoRotateDebug = true;
constexpr uint32_t kDisplayAutoRotateDebugMs = 1000;
constexpr uint8_t kDefaultBrightness = 210;
constexpr uint8_t kBootButtonPin = 0;

const RemoteBoardPins kBoardPins = [] {
    RemoteBoardPins pins = {};
    pins.displayData0 = 14;
    pins.displayData1 = 10;
    pins.displayData2 = 16;
    pins.displayData3 = 12;
    pins.displaySclk = 15;
    pins.displayCs = 11;
    pins.displayReset = 13;
    pins.displayTe = 18;
    pins.touchSda = 6;
    pins.touchScl = 7;
    pins.touchInt = 8;
    pins.touchReset = 17;
    pins.pmicInt = 5;
    pins.powerEnable = 9;
    pins.primaryButton = kBootButtonPin;
    pins.expansionSpiMiso = 4;
    pins.expansionSpiMosi = 2;
    pins.expansionSpiSclk = 3;
    pins.expansionSpiCs = 1;
    return pins;
}();

RemoteBoardServices makeBoardServices() {
    RemoteBoardServices services = {};
    services.i2c = &Wire;
    services.console = &Serial;
    services.i2cSda = kBoardPins.touchSda;
    services.i2cScl = kBoardPins.touchScl;
    services.expansionSpi = &SPI;
    services.expansionSpiSclk = kBoardPins.expansionSpiSclk;
    services.expansionSpiMosi = kBoardPins.expansionSpiMosi;
    services.expansionSpiMiso = kBoardPins.expansionSpiMiso;
    services.expansionSpiCs = kBoardPins.expansionSpiCs;
    services.consoleIsUsb = true;
    return services;
}

struct ButtonTracker {
    bool initialized = false;
    bool stablePressed = false;
    bool lastSamplePressed = false;
    uint32_t debounceStartedAt = 0;
    uint32_t stableChangedAt = 0;
    bool longLatched = false;
};

class LilyGoFramebuffer : public Adafruit_GFX {
  public:
    LilyGoFramebuffer() : Adafruit_GFX(kPanelNativeWidth, kPanelNativeHeight) {}

    bool begin() {
        if(framebuffer_ != nullptr) {
            return true;
        }

        const size_t pixelCount = static_cast<size_t>(kPanelNativeWidth) *
                                  static_cast<size_t>(kPanelNativeHeight);
        const size_t bufferBytes = pixelCount * sizeof(uint16_t);
        framebuffer_ = static_cast<uint16_t*>(
            heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        usingPsram_ = framebuffer_ != nullptr;
        if(framebuffer_ == nullptr) {
            framebuffer_ = static_cast<uint16_t*>(heap_caps_malloc(bufferBytes, MALLOC_CAP_8BIT));
        }
        if(framebuffer_ == nullptr) {
            return false;
        }
        fillScreen(0x0000);
        dirty_ = true;
        return true;
    }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        writePixel(x, y, color);
    }

    void writePixel(int16_t x, int16_t y, uint16_t color) override {
        if(framebuffer_ == nullptr || x < 0 || y < 0 || x >= _width || y >= _height) {
            return;
        }

        mapLogicalToFramebuffer(&x, &y);
        if(x < 0 || y < 0 || x >= kPanelNativeWidth || y >= kPanelNativeHeight) {
            return;
        }
        framebuffer_[static_cast<size_t>(y) * kPanelNativeWidth + x] = color;
        dirty_ = true;
    }

    void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        writeFillRect(x, y, 1, h, color);
    }

    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        writeFillRect(x, y, w, 1, color);
    }

    void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        if(framebuffer_ == nullptr || w <= 0 || h <= 0) {
            return;
        }

        if(x < 0) {
            w = static_cast<int16_t>(w + x);
            x = 0;
        }
        if(y < 0) {
            h = static_cast<int16_t>(h + y);
            y = 0;
        }
        if(x >= _width || y >= _height || w <= 0 || h <= 0) {
            return;
        }
        if(x + w > _width) {
            w = static_cast<int16_t>(_width - x);
        }
        if(y + h > _height) {
            h = static_cast<int16_t>(_height - y);
        }
        if(w <= 0 || h <= 0) {
            return;
        }

        int16_t px = x;
        int16_t py = y;
        int16_t pw = w;
        int16_t ph = h;
        switch(getRotation() & 0x03) {
            case 1:
                px = static_cast<int16_t>(kPanelNativeWidth - y - h);
                py = x;
                pw = h;
                ph = w;
                break;
            case 2:
                px = static_cast<int16_t>(kPanelNativeWidth - x - w);
                py = static_cast<int16_t>(kPanelNativeHeight - y - h);
                break;
            case 3:
                px = y;
                py = static_cast<int16_t>(kPanelNativeHeight - x - w);
                pw = h;
                ph = w;
                break;
            default:
                break;
        }

        fillFramebufferRect(px, py, pw, ph, color);
        dirty_ = true;
    }

    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        writeFillRect(x, y, 1, h, color);
    }

    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        writeFillRect(x, y, w, 1, color);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        writeFillRect(x, y, w, h, color);
    }

    void fillScreen(uint16_t color) override {
        fillFramebufferRect(0, 0, kPanelNativeWidth, kPanelNativeHeight, color);
        dirty_ = true;
    }

    uint16_t* framebuffer() {
        return framebuffer_;
    }

    bool dirty() const {
        return dirty_;
    }

    void markClean() {
        dirty_ = false;
    }

    bool usingPsram() const {
        return usingPsram_;
    }

  private:
    void mapLogicalToFramebuffer(int16_t* x, int16_t* y) const {
        const int16_t ox = *x;
        const int16_t oy = *y;
        switch(getRotation() & 0x03) {
            case 1:
                *x = static_cast<int16_t>(kPanelNativeWidth - 1 - oy);
                *y = ox;
                break;
            case 2:
                *x = static_cast<int16_t>(kPanelNativeWidth - 1 - ox);
                *y = static_cast<int16_t>(kPanelNativeHeight - 1 - oy);
                break;
            case 3:
                *x = oy;
                *y = static_cast<int16_t>(kPanelNativeHeight - 1 - ox);
                break;
            default:
                break;
        }
    }

    void fillFramebufferRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        if(framebuffer_ == nullptr || w <= 0 || h <= 0) {
            return;
        }

        if(x < 0) {
            w = static_cast<int16_t>(w + x);
            x = 0;
        }
        if(y < 0) {
            h = static_cast<int16_t>(h + y);
            y = 0;
        }
        if(x >= kPanelNativeWidth || y >= kPanelNativeHeight || w <= 0 || h <= 0) {
            return;
        }
        if(x + w > kPanelNativeWidth) {
            w = static_cast<int16_t>(kPanelNativeWidth - x);
        }
        if(y + h > kPanelNativeHeight) {
            h = static_cast<int16_t>(kPanelNativeHeight - y);
        }
        if(w <= 0 || h <= 0) {
            return;
        }

        if(x == 0 && y == 0 && w == kPanelNativeWidth && h == kPanelNativeHeight) {
            const size_t pixelCount = static_cast<size_t>(kPanelNativeWidth) *
                                      static_cast<size_t>(kPanelNativeHeight);
            if(color == 0) {
                memset(framebuffer_, 0, pixelCount * sizeof(uint16_t));
            } else {
                for(size_t i = 0; i < pixelCount; ++i) {
                    framebuffer_[i] = color;
                }
            }
            return;
        }

        for(int16_t row = 0; row < h; ++row) {
            uint16_t* line = framebuffer_ +
                             (static_cast<size_t>(y + row) * kPanelNativeWidth) + x;
            for(int16_t col = 0; col < w; ++col) {
                line[col] = color;
            }
        }
    }

    uint16_t* framebuffer_ = nullptr;
    bool dirty_ = false;
    bool usingPsram_ = false;
};

Arduino_ESP32QSPI* g_bus = nullptr;
Arduino_RM690B0* g_display = nullptr;
TouchDrvCSTXXX g_touch;
PowersSY6970 g_pmic;
SensorQMI8658 g_imu;
LilyGoFramebuffer g_framebuffer;
bool g_displayReady = false;

class LilyGoCanvas : public RemoteCanvas {
  public:
    uint16_t width() const override {
        return static_cast<uint16_t>(g_framebuffer.width());
    }

    uint16_t height() const override {
        return static_cast<uint16_t>(g_framebuffer.height());
    }

    void fillScreen(uint16_t color) override {
        g_framebuffer.fillScreen(color);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        g_framebuffer.fillRect(x, y, w, h, color);
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        g_framebuffer.drawRect(x, y, w, h, color);
    }

    void fillRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        g_framebuffer.fillRoundRect(x, y, w, h, radius, color);
    }

    void drawRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        g_framebuffer.drawRoundRect(x, y, w, h, radius, color);
    }

    void fillCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        g_framebuffer.fillCircle(x, y, radius, color);
    }

    void drawCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        g_framebuffer.drawCircle(x, y, radius, color);
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
        g_framebuffer.drawLine(x0, y0, x1, y1, color);
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
        g_framebuffer.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        g_framebuffer.setCursor(static_cast<int16_t>(x - static_cast<int16_t>(w / 2)),
                                static_cast<int16_t>(y - static_cast<int16_t>(h / 2)));
        g_framebuffer.print(text);
    }

    void drawTextTopLeft(const String& text,
                         int16_t x,
                         int16_t y,
                         RemoteTextStyle style,
                         uint16_t textColor,
                         uint16_t bgColor) override {
        configureText(style, textColor, bgColor);
        g_framebuffer.setCursor(x, static_cast<int16_t>(y + baselineOffset(style)));
        g_framebuffer.print(text);
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
        g_framebuffer.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        g_framebuffer.setCursor(static_cast<int16_t>(x - static_cast<int16_t>(w)),
                                static_cast<int16_t>(y + baselineOffset(style)));
        g_framebuffer.print(text);
    }

    void present() override {
        if(!g_displayReady || g_framebuffer.framebuffer() == nullptr || !g_framebuffer.dirty()) {
            return;
        }
        g_display->draw16bitRGBBitmap(0,
                                      0,
                                      g_framebuffer.framebuffer(),
                                      kPanelNativeWidth,
                                      kPanelNativeHeight);
        g_framebuffer.markClean();
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
        g_framebuffer.setFont(nullptr);
        g_framebuffer.setTextSize(sizeForStyle(style));
        g_framebuffer.setTextWrap(false);
        g_framebuffer.setTextColor(textColor, bgColor);
    }
};

LilyGoCanvas g_canvas;
RemoteTouchState g_touchState;
ButtonTracker g_bootButton;
RemoteBoardServices g_boardServices = makeBoardServices();
volatile bool g_bootButtonInterruptPending = true;
volatile bool g_touchInterruptPending = true;
volatile bool g_pmicInterruptPending = true;
uint32_t g_lastButtonPollAt = 0;
uint32_t g_lastTouchPollAt = 0;
uint32_t g_touchLastGoodAt = 0;
uint32_t g_lastPmicStatusPollAt = 0;
uint32_t g_imuLastPollAt = 0;
uint32_t g_displayAutoRotateLastSampleAt = 0;
uint32_t g_displayAutoRotateLastDebugAt = 0;
uint8_t g_displayRotation = kDisplayRotation;
uint8_t g_displayRotationCandidate = kDisplayRotation;
uint8_t g_displayRotationCandidateCount = 0;
bool g_touchReady = false;
bool g_pmicReady = false;
bool g_imuReady = false;
bool g_expansionSpiReady = false;
bool g_haveBatteryStatus = false;
uint8_t g_lastPmicFaultStatus = 0;
RemoteBatteryStatus g_lastBatteryStatus;

const RemoteBoardInfo kBoardInfo = {
    RemoteBoardId::LilyGoT4S3H678,
    "LILYGO T4-S3 H678 2.41 AMOLED",
    kDisplayWidth,
    kDisplayHeight,
    true,
    false,
        static_cast<uint32_t>(RemoteBoardCapability::Touch) |
        static_cast<uint32_t>(RemoteBoardCapability::Backlight) |
        static_cast<uint32_t>(RemoteBoardCapability::I2c) |
        static_cast<uint32_t>(RemoteBoardCapability::Spi) |
        static_cast<uint32_t>(RemoteBoardCapability::BatterySense) |
        static_cast<uint32_t>(RemoteBoardCapability::Pmic) |
        static_cast<uint32_t>(RemoteBoardCapability::DeepSleep) |
        static_cast<uint32_t>(RemoteBoardCapability::WakeSource) |
        static_cast<uint32_t>(RemoteBoardCapability::ExpansionSpi) |
        static_cast<uint32_t>(RemoteBoardCapability::UsbDetect) |
        static_cast<uint32_t>(RemoteBoardCapability::ChargeState) |
        static_cast<uint32_t>(RemoteBoardCapability::UsbConsole),
};

RemoteBoardFeatureMap g_boardFeatureMap = {
    {
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Implemented,
        static_cast<uint32_t>(RemoteWakeSource::Touch) |
            static_cast<uint32_t>(RemoteWakeSource::PrimaryButton),
    },
    {
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Mapped,
    },
    {},
    {},
    {},
    {
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::None,
        0,
    },
    {
        RemoteBoardSupportLevel::Implemented,
        true,
    },
    {
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::None,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::None,
        true,
        true,
    },
};

void IRAM_ATTR bootButtonInterruptHandler() {
    g_bootButtonInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

void IRAM_ATTR touchInterruptHandler() {
    g_touchInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

void IRAM_ATTR pmicInterruptHandler() {
    g_pmicInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

bool beginSharedI2cBus() {
#if defined(ARDUINO_ARCH_ESP32)
    Wire.end();
    delay(2);
#endif
    const bool started = Wire.begin(kBoardPins.touchSda, kBoardPins.touchScl, kI2cClock);
    Wire.setTimeOut(20);
    Wire.setTimeout(20);
    return started;
}

bool probeI2cDevice(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

void scanSharedI2cBus() {
    char detected[128] = {};
    size_t used = 0;
    for(uint8_t address = 0x08; address < 0x78; ++address) {
        if(!probeI2cDevice(address)) {
            continue;
        }
        const int written = snprintf(detected + used,
                                     sizeof(detected) - used,
                                     "%s0x%02X",
                                     used == 0 ? "" : " ",
                                     address);
        if(written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
        if(used >= sizeof(detected)) {
            break;
        }
    }
    Serial.printf("[remote] lilygo i2c scan: %s\n", used == 0 ? "none" : detected);
}

void initButtonTracker(ButtonTracker* tracker, bool pressed) {
    if(tracker == nullptr) {
        return;
    }

    tracker->initialized = true;
    tracker->stablePressed = pressed;
    tracker->lastSamplePressed = pressed;
    tracker->debounceStartedAt = millis();
    tracker->stableChangedAt = tracker->debounceStartedAt;
    tracker->longLatched = false;
}

void updateButtonTracker(ButtonTracker* tracker,
                         bool pressed,
                         bool* justReleased,
                         bool* longPress) {
    if(justReleased != nullptr) {
        *justReleased = false;
    }
    if(longPress != nullptr) {
        *longPress = false;
    }
    if(tracker == nullptr) {
        return;
    }

    const uint32_t now = millis();
    if(!tracker->initialized) {
        initButtonTracker(tracker, pressed);
        return;
    }

    if(pressed != tracker->lastSamplePressed) {
        tracker->lastSamplePressed = pressed;
        tracker->debounceStartedAt = now;
    }

    if(now - tracker->debounceStartedAt < kButtonDebounceMs) {
        return;
    }

    if(pressed == tracker->stablePressed) {
        if(tracker->stablePressed && !tracker->longLatched &&
           now - tracker->stableChangedAt >= kLongPressMs) {
            tracker->longLatched = true;
            if(longPress != nullptr) {
                *longPress = true;
            }
        }
        return;
    }

    tracker->stablePressed = pressed;
    tracker->stableChangedAt = now;
    if(pressed) {
        tracker->longLatched = false;
    } else {
        if(!tracker->longLatched && justReleased != nullptr) {
            *justReleased = true;
        }
        tracker->longLatched = false;
    }
}

uint16_t currentTouchWidth() {
    return static_cast<uint16_t>(g_framebuffer.width());
}

uint16_t currentTouchHeight() {
    return static_cast<uint16_t>(g_framebuffer.height());
}

void applyTouchRotationConfig(uint8_t rotation) {
    if(!g_touchReady) {
        return;
    }

    switch(rotation & 0x03) {
        case 1:
            g_touch.setMaxCoordinates(kDisplayWidth, kDisplayHeight);
            g_touch.setSwapXY(false);
            g_touch.setMirrorXY(false, false);
            break;
        case 2:
            g_touch.setMaxCoordinates(kPanelNativeWidth, kPanelNativeHeight);
            g_touch.setSwapXY(true);
            g_touch.setMirrorXY(true, false);
            break;
        case 3:
            g_touch.setMaxCoordinates(kDisplayWidth, kDisplayHeight);
            g_touch.setSwapXY(false);
            g_touch.setMirrorXY(true, true);
            break;
        default:
            g_touch.setMaxCoordinates(kPanelNativeWidth, kPanelNativeHeight);
            g_touch.setSwapXY(true);
            g_touch.setMirrorXY(false, true);
            break;
    }
}

bool readTouchPoint(int16_t* x, int16_t* y) {
    if(x == nullptr || y == nullptr || !g_touchReady) {
        return false;
    }

    int16_t xs[1] = {0};
    int16_t ys[1] = {0};
    const uint8_t points = g_touch.getPoint(xs, ys, 1);
    if(points == 0) {
        return false;
    }
    if(xs[0] < 0 || ys[0] < 0) {
        return false;
    }

    *x = min<int16_t>(xs[0], static_cast<int16_t>(currentTouchWidth() - 1));
    *y = min<int16_t>(ys[0], static_cast<int16_t>(currentTouchHeight() - 1));
    return true;
}

bool applyDisplayRotation(uint8_t rotation) {
    rotation &= 0x03;
    if(rotation == g_displayRotation) {
        return false;
    }

    g_displayRotation = rotation;
    g_displayRotationCandidate = rotation;
    g_displayRotationCandidateCount = 0;
    g_framebuffer.setRotation(rotation);
    applyTouchRotationConfig(rotation);
    g_framebuffer.fillScreen(0x0000);
    Serial.printf("[remote] lilygo display rotation=%u size=%ux%u\n",
                  static_cast<unsigned>(rotation),
                  static_cast<unsigned>(g_framebuffer.width()),
                  static_cast<unsigned>(g_framebuffer.height()));
    return true;
}

uint8_t correctedDisplayRotation(uint8_t rotation) {
    return static_cast<uint8_t>((rotation + kDisplayAutoRotateCorrection) & 0x03);
}

uint8_t displayRotationFromGravity(float ax, float ay, float az) {
    const float absX = fabsf(ax);
    const float absY = fabsf(ay);
    const float absZ = fabsf(az);
    const float dominant = absX > absY ? absX : absY;
    if(dominant < kDisplayAutoRotateDominantG) {
        return g_displayRotation;
    }
    if(absZ >= dominant - kDisplayAutoRotateFlatRejectMarginG) {
        return g_displayRotation;
    }
    if(absY >= absX + kDisplayAutoRotateAxisMarginG) {
        return correctedDisplayRotation(ay > 0.0f ? 1 : 3);
    }
    if(absX >= absY + kDisplayAutoRotateAxisMarginG) {
        return correctedDisplayRotation(ax > 0.0f ? 2 : 0);
    }
    return g_displayRotation;
}

void debugDisplayAutoRotate(uint32_t now,
                            const char* status,
                            float ax = 0.0f,
                            float ay = 0.0f,
                            float az = 0.0f,
                            uint8_t rotation = 0xFF) {
    if(!kDisplayAutoRotateDebug ||
       static_cast<uint32_t>(now - g_displayAutoRotateLastDebugAt) <
           kDisplayAutoRotateDebugMs) {
        return;
    }
    g_displayAutoRotateLastDebugAt = now;
    Serial.printf("[remote][imu] %s ax=%d ay=%d az=%d cur=%u next=%u\n",
                  status != nullptr ? status : "?",
                  static_cast<int>(ax * 1000.0f),
                  static_cast<int>(ay * 1000.0f),
                  static_cast<int>(az * 1000.0f),
                  static_cast<unsigned>(g_displayRotation),
                  rotation == 0xFF ? 9u : static_cast<unsigned>(rotation));
}

bool readImuAccelerometerG(float* ax, float* ay, float* az) {
    if(ax == nullptr || ay == nullptr || az == nullptr || !g_imuReady) {
        return false;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if(g_imu.getAccelerometer(x, y, z) &&
       (fabsf(x) + fabsf(y) + fabsf(z)) > 0.05f) {
        *ax = x;
        *ay = y;
        *az = z;
        return true;
    }

    int16_t raw[3] = {};
    if(!g_imu.getAccelRaw(raw)) {
        return false;
    }

    *ax = static_cast<float>(raw[0]) * kImuAccelScaleG;
    *ay = static_cast<float>(raw[1]) * kImuAccelScaleG;
    *az = static_cast<float>(raw[2]) * kImuAccelScaleG;
    return (fabsf(*ax) + fabsf(*ay) + fabsf(*az)) > 0.05f;
}

void pollTouch(RemoteInputState* input) {
    if(input == nullptr || !g_touchReady) {
        return;
    }

    const uint32_t now = millis();
    const bool irqActive = digitalRead(kBoardPins.touchInt) == LOW;
    const uint32_t pollInterval = g_touchState.pressed ? kTouchPollMs : kTouchIdlePollMs;
    const bool timedPollDue = (now - g_lastTouchPollAt) >= pollInterval;
    const bool needsPoll = g_touchInterruptPending || irqActive || g_touchState.pressed || timedPollDue;
    if(!needsPoll || (!timedPollDue && !g_touchInterruptPending && !irqActive)) {
        input->touch = g_touchState;
        input->touch.justPressed = false;
        input->touch.justReleased = false;
        return;
    }

    g_lastTouchPollAt = now;
    int16_t x = g_touchState.x;
    int16_t y = g_touchState.y;
    const bool touched = readTouchPoint(&x, &y);
    input->touch = {};

    if(touched) {
        g_touchLastGoodAt = now;
        input->touch.pressed = true;
        input->touch.x = x;
        input->touch.y = y;
        if(!g_touchState.pressed) {
            input->touch.justPressed = true;
            Serial.printf("[remote][input] touch press x=%d y=%d irq=%d\n",
                          static_cast<int>(x),
                          static_cast<int>(y),
                          irqActive ? 1 : 0);
        }
        g_touchState.pressed = true;
        g_touchState.x = x;
        g_touchState.y = y;
        return;
    }

    if(!irqActive) {
        g_touchInterruptPending = false;
    }
    if(g_touchState.pressed && now - g_touchLastGoodAt >= kTouchReleaseHoldMs) {
        input->touch.justReleased = true;
        input->touch.x = g_touchState.x;
        input->touch.y = g_touchState.y;
        Serial.printf("[remote][input] touch release x=%d y=%d\n",
                      static_cast<int>(g_touchState.x),
                      static_cast<int>(g_touchState.y));
        g_touchState.pressed = false;
        return;
    }

    input->touch = g_touchState;
    input->touch.justPressed = false;
    input->touch.justReleased = false;
}

int8_t batteryPercentFromMillivolts(int32_t millivolts) {
    if(millivolts <= 0) {
        return -1;
    }
    if(millivolts <= 3300) {
        return 0;
    }
    if(millivolts >= 4200) {
        return 100;
    }
    return static_cast<int8_t>((millivolts - 3300) * 100 / 900);
}

int32_t batteryAbsDiff(int32_t lhs, int32_t rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

bool batteryValueClose(int32_t lhs, int32_t rhs, int32_t tolerance) {
    if(lhs < 0 || rhs < 0) {
        return lhs == rhs;
    }
    return batteryAbsDiff(lhs, rhs) <= tolerance;
}

bool batteryTopOffUsbHold(const RemoteBatteryStatus& status) {
    return status.usbPresent &&
           status.batteryPresent &&
           status.percent >= kBatteryTopOffPercent &&
           status.millivolts >= kBatteryTopOffMv;
}

void normalizePmicBatteryStatus(RemoteBatteryStatus* status) {
    if(status == nullptr) {
        return;
    }

    if(status->usbPresent) {
        // The SY6970 charge-status and battery-voltage readings chatter while
        // USB is present. For the UI, the useful state is simply USB power.
        status->charging = false;
        status->standby = true;
        status->discharging = false;
        status->percent = -1;
        status->millivolts = -1;
        status->systemMillivolts = -1;
        return;
    }

    status->charging = false;
    status->standby = false;
    status->discharging = status->batteryPresent;
}

RemoteBatteryStatus readPmicBatteryStatus() {
    RemoteBatteryStatus status = {};
    status.supported = g_pmicReady;
    if(!g_pmicReady) {
        return status;
    }

    const uint16_t battMv = g_pmic.getBattVoltage();
    const uint16_t vbusMv = g_pmic.getVbusVoltage();
    const uint16_t sysMv = g_pmic.getSystemVoltage();
    status.batteryPresent = battMv > 2500;
    status.usbPresent = g_pmic.isVbusIn() || vbusMv > 3500;
    status.charging = g_pmic.isCharging();
    status.discharging = status.batteryPresent && !status.charging && !status.usbPresent;
    status.standby = status.usbPresent && !status.charging;
    status.millivolts = status.batteryPresent ? battMv : -1;
    status.vbusMillivolts = vbusMv > 0 ? vbusMv : -1;
    status.systemMillivolts = sysMv > 0 ? sysMv : -1;
    status.percent = batteryPercentFromMillivolts(status.millivolts);
    normalizePmicBatteryStatus(&status);
    return status;
}

bool batteryStatusEquals(const RemoteBatteryStatus& lhs, const RemoteBatteryStatus& rhs) {
    const bool topOffEquivalent = batteryTopOffUsbHold(lhs) && batteryTopOffUsbHold(rhs);
    const bool chargeStateEqual =
        topOffEquivalent ||
        (lhs.charging == rhs.charging &&
         lhs.discharging == rhs.discharging &&
         lhs.standby == rhs.standby);

    return lhs.supported == rhs.supported && lhs.batteryPresent == rhs.batteryPresent &&
           lhs.usbPresent == rhs.usbPresent && chargeStateEqual &&
           batteryValueClose(lhs.percent, rhs.percent, kBatteryPercentJitter) &&
           batteryValueClose(lhs.millivolts, rhs.millivolts, kBatteryVoltageJitterMv) &&
           batteryValueClose(lhs.vbusMillivolts, rhs.vbusMillivolts, kBatteryVbusJitterMv) &&
           batteryValueClose(lhs.systemMillivolts, rhs.systemMillivolts, kBatteryVoltageJitterMv);
}

RemoteBatteryStatus cachedPmicBatteryStatus() {
    if(!g_pmicReady) {
        return readPmicBatteryStatus();
    }

    const uint32_t now = millis();
    if(g_haveBatteryStatus && (now - g_lastPmicStatusPollAt) < kPmicStatusPollMs) {
        return g_lastBatteryStatus;
    }

    g_lastPmicStatusPollAt = now;
    g_lastBatteryStatus = readPmicBatteryStatus();
    g_haveBatteryStatus = true;
    return g_lastBatteryStatus;
}

void updateFeatureMapAfterInit() {
    const RemoteBoardSupportLevel touchLevel =
        g_touchReady ? RemoteBoardSupportLevel::Implemented : RemoteBoardSupportLevel::Mapped;
    const RemoteBoardSupportLevel pmicLevel =
        g_pmicReady ? RemoteBoardSupportLevel::Implemented : RemoteBoardSupportLevel::Mapped;
    const RemoteBoardSupportLevel spiLevel =
        g_expansionSpiReady ? RemoteBoardSupportLevel::Implemented : RemoteBoardSupportLevel::Mapped;
    const RemoteBoardSupportLevel imuLevel =
        g_imuReady ? RemoteBoardSupportLevel::Implemented : RemoteBoardSupportLevel::None;

    g_boardFeatureMap.power.pmic = pmicLevel;
    g_boardFeatureMap.battery.voltage = pmicLevel;
    g_boardFeatureMap.battery.usbPresent = pmicLevel;
    g_boardFeatureMap.battery.chargeState = pmicLevel;
    g_boardFeatureMap.motion.imu = imuLevel;
    g_boardFeatureMap.expansion.spi = spiLevel;
    g_boardFeatureMap.input.touch = touchLevel;
}

bool initExpansionSpiSlot() {
    if(kBoardPins.expansionSpiSclk < 0 || kBoardPins.expansionSpiMiso < 0 ||
       kBoardPins.expansionSpiMosi < 0 || kBoardPins.expansionSpiCs < 0) {
        return false;
    }

    SPI.begin(kBoardPins.expansionSpiSclk,
              kBoardPins.expansionSpiMiso,
              kBoardPins.expansionSpiMosi,
              kBoardPins.expansionSpiCs);
    pinMode(kBoardPins.expansionSpiCs, OUTPUT);
    digitalWrite(kBoardPins.expansionSpiCs, HIGH);
    return true;
}

bool initImuAt(uint8_t address) {
    if(!probeI2cDevice(address)) {
        return false;
    }

    Wire.end();
    delay(2);
    if(!g_imu.begin(Wire, address, kBoardPins.touchSda, kBoardPins.touchScl)) {
        beginSharedI2cBus();
        return false;
    }
    if(!g_imu.configAccelerometer(kImuAccelRange, kImuWakeOdr, SensorQMI8658::LPF_MODE_0)) {
        beginSharedI2cBus();
        return false;
    }
    if(!g_imu.enableAccelerometer()) {
        beginSharedI2cBus();
        return false;
    }
    beginSharedI2cBus();
    Serial.printf("[remote] lilygo imu ready addr=0x%02X accel=%.1fHz range=8g\n",
                  address,
                  kImuWakeOdrLabelHz);
    return true;
}

bool initImuMotion() {
    const bool lowPresent = probeI2cDevice(QMI8658_L_SLAVE_ADDRESS);
    constexpr bool highAddressReservedForPmic = QMI8658_H_SLAVE_ADDRESS == kPmicAddress;
    const bool highPresent =
        highAddressReservedForPmic ? false : probeI2cDevice(QMI8658_H_SLAVE_ADDRESS);
    Serial.printf("[remote] lilygo imu probe 0x%02X=%d 0x%02X=%s\n",
                  QMI8658_L_SLAVE_ADDRESS,
                  lowPresent ? 1 : 0,
                  QMI8658_H_SLAVE_ADDRESS,
                  highAddressReservedForPmic ? "reserved-pmic" : (highPresent ? "1" : "0"));

    if(initImuAt(kImuDefaultAddress)) {
        return true;
    }
    if(highAddressReservedForPmic) {
        Serial.println("[remote] lilygo imu not present");
        return false;
    }
    const uint8_t fallback = kImuDefaultAddress == QMI8658_L_SLAVE_ADDRESS
                                 ? QMI8658_H_SLAVE_ADDRESS
                                 : QMI8658_L_SLAVE_ADDRESS;
    return initImuAt(fallback);
}

void serviceDisplayAutoRotate(RemoteInputState* input) {
    if(input == nullptr || !remote_config::kDisplayAutoRotate || !g_imuReady) {
        return;
    }

    const uint32_t now = millis();
    if(static_cast<uint32_t>(now - g_displayAutoRotateLastSampleAt) <
       remote_config::kDisplayAutoRotateSampleMs) {
        return;
    }
    g_displayAutoRotateLastSampleAt = now;
    g_imuLastPollAt = now;

    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    if(!readImuAccelerometerG(&ax, &ay, &az)) {
        debugDisplayAutoRotate(now, "read_fail");
        return;
    }

    const uint8_t rotation = displayRotationFromGravity(ax, ay, az);
    if(rotation == g_displayRotation) {
        g_displayRotationCandidate = rotation;
        g_displayRotationCandidateCount = 0;
        debugDisplayAutoRotate(now, "hold", ax, ay, az, rotation);
        return;
    }

    if(rotation != g_displayRotationCandidate) {
        g_displayRotationCandidate = rotation;
        g_displayRotationCandidateCount = 1;
        debugDisplayAutoRotate(now, "candidate", ax, ay, az, rotation);
        return;
    }
    if(g_displayRotationCandidateCount < kDisplayAutoRotateConfirmSamples) {
        ++g_displayRotationCandidateCount;
    }
    if(g_displayRotationCandidateCount < kDisplayAutoRotateConfirmSamples) {
        debugDisplayAutoRotate(now, "candidate", ax, ay, az, rotation);
        return;
    }

    debugDisplayAutoRotate(now, "apply", ax, ay, az, rotation);
    if(applyDisplayRotation(rotation)) {
        input->displayRotationChanged = true;
        input->displayRotation = rotation;
    }
}

void pollPmic(RemoteInputState* input) {
    if(input == nullptr || !g_pmicReady) {
        return;
    }

    if(!g_pmicInterruptPending) {
        return;
    }

    const uint32_t now = millis();
    if(g_haveBatteryStatus && (now - g_lastPmicStatusPollAt) < kPmicStatusPollMs) {
        return;
    }

    const RemoteBatteryStatus status = readPmicBatteryStatus();
    const uint8_t faultStatus = g_pmic.getFaultStatus();
    const bool statusChanged = !g_haveBatteryStatus ||
                               !batteryStatusEquals(status, g_lastBatteryStatus) ||
                               faultStatus != g_lastPmicFaultStatus;
    input->powerStatusChanged = statusChanged;

    g_lastBatteryStatus = status;
    g_lastPmicFaultStatus = faultStatus;
    g_haveBatteryStatus = true;
    g_lastPmicStatusPollAt = now;
    g_pmicInterruptPending = false;
}

bool ensureDisplayObjects() {
    if(g_display != nullptr) {
        return true;
    }

    if(kBoardPins.powerEnable >= 0) {
        pinMode(kBoardPins.powerEnable, OUTPUT);
        digitalWrite(kBoardPins.powerEnable, HIGH);
        delay(20);
    }
    g_bus = new(std::nothrow) Arduino_ESP32QSPI(
        kBoardPins.displayCs,
        kBoardPins.displaySclk,
        kBoardPins.displayData0,
        kBoardPins.displayData1,
        kBoardPins.displayData2,
        kBoardPins.displayData3);
    if(g_bus == nullptr) {
        Serial.println("[remote] lilygo display bus alloc failed");
        return false;
    }

    g_display = new(std::nothrow) Arduino_RM690B0(
        g_bus,
        kBoardPins.displayReset,
        0,
        RM690B0_TFTWIDTH,
        RM690B0_TFTHEIGHT,
        0,
        16,
        0,
        0);
    if(g_display == nullptr) {
        Serial.println("[remote] lilygo display alloc failed");
        delete g_bus;
        g_bus = nullptr;
        return false;
    }

    return true;
}

bool lilygo_probe() {
#if defined(REMOTE_BOARD_FORCE_LILYGO_T4_S3_H678)
    return true;
#else
    beginSharedI2cBus();
    const bool touchReady = probeI2cDevice(kTouchAddress);
    const bool pmicReady = probeI2cDevice(kPmicAddress);
    if(touchReady || pmicReady) {
        Serial.printf("[remote] lilygo probe touch=%d pmic=%d\n",
                      touchReady ? 1 : 0,
                      pmicReady ? 1 : 0);
    }
    return touchReady || pmicReady;
#endif
}

bool lilygo_init() {
    pinMode(kBoardPins.primaryButton, INPUT_PULLUP);
    pinMode(kBoardPins.touchInt, INPUT_PULLUP);
    pinMode(kBoardPins.pmicInt, INPUT_PULLUP);
    beginSharedI2cBus();
    scanSharedI2cBus();
    g_expansionSpiReady = initExpansionSpiSlot();

    if(!g_framebuffer.begin()) {
        Serial.println("[remote] lilygo framebuffer alloc failed");
        return false;
    }

    if(!ensureDisplayObjects() || !g_display->begin(36000000)) {
        Serial.println("[remote] lilygo display init failed");
        return false;
    }

    g_display->setRotation(0);
    g_framebuffer.setRotation(kDisplayRotation);
    g_displayRotation = kDisplayRotation;
    g_displayRotationCandidate = kDisplayRotation;
    g_displayAutoRotateLastSampleAt = millis();
    g_display->setBrightness(kDefaultBrightness);
    g_displayReady = true;

    if(probeI2cDevice(kTouchAddress)) {
        g_touch.setTouchDrvModel(TouchDrv_CST226);
        g_touch.setPins(kBoardPins.touchReset, kBoardPins.touchInt);
        const bool touchInitOk = g_touch.begin(Wire, kTouchAddress, -1, -1);
        g_touchReady = touchInitOk && g_touch.getSupportTouchPoint() > 0;
        if(g_touchReady) {
            applyTouchRotationConfig(g_displayRotation);
        } else {
            Serial.println("[remote] lilygo CST226SE init failed");
        }
    }

    g_imuReady = initImuMotion();

    if(probeI2cDevice(kPmicAddress) &&
       g_pmic.init(Wire, kBoardPins.touchSda, kBoardPins.touchScl, kPmicAddress)) {
        g_pmic.enableMeasure();
        g_pmic.disableOTG();
        g_pmic.disableStatLed();
        g_pmicReady = true;
        g_lastBatteryStatus = readPmicBatteryStatus();
        g_lastPmicFaultStatus = g_pmic.getFaultStatus();
        g_haveBatteryStatus = true;
        g_lastPmicStatusPollAt = millis();
    }

    attachInterrupt(kBoardPins.primaryButton, bootButtonInterruptHandler, CHANGE);
    if(g_touchReady) {
        attachInterrupt(kBoardPins.touchInt, touchInterruptHandler, FALLING);
    }
    initButtonTracker(&g_bootButton, digitalRead(kBoardPins.primaryButton) == LOW);
    g_bootButtonInterruptPending = true;
    g_lastButtonPollAt = millis();
    g_touchInterruptPending = g_touchReady;
    g_pmicInterruptPending = false;
    g_touchState = {};
    updateFeatureMapAfterInit();
    Serial.printf("[remote] lilygo ready %ux%u rotation=%u psram=%d touch=%d pmic=%d imu=%d spi=%d button0=gpio%d\n",
                  static_cast<unsigned>(g_framebuffer.width()),
                  static_cast<unsigned>(g_framebuffer.height()),
                  static_cast<unsigned>(g_displayRotation),
                  g_framebuffer.usingPsram() ? 1 : 0,
                  g_touchReady ? 1 : 0,
                  g_pmicReady ? 1 : 0,
                  g_imuReady ? 1 : 0,
                  g_expansionSpiReady ? 1 : 0,
                  static_cast<int>(kBoardPins.primaryButton));
    return true;
}

void lilygo_poll_input(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    *input = {};

    const uint32_t now = millis();
    const bool buttonPollDue = (now - g_lastButtonPollAt) >= kButtonPollMs;
    bool primaryShortPress = false;
    bool primaryLongPress = false;
    if(g_bootButtonInterruptPending || buttonPollDue || !g_bootButton.initialized ||
       g_bootButton.stablePressed) {
        g_lastButtonPollAt = now;
        const bool wasPressed = g_bootButton.stablePressed;
        updateButtonTracker(&g_bootButton,
                            digitalRead(kBoardPins.primaryButton) == LOW,
                            &primaryShortPress,
                            &primaryLongPress);
        if(g_bootButton.initialized && wasPressed != g_bootButton.stablePressed) {
            Serial.printf("[remote][input] boot gpio%d %s\n",
                          static_cast<int>(kBoardPins.primaryButton),
                          g_bootButton.stablePressed ? "down" : "up");
        }
    }
    if(g_bootButton.initialized &&
       g_bootButton.lastSamplePressed == g_bootButton.stablePressed &&
       !g_bootButton.stablePressed) {
        g_bootButtonInterruptPending = false;
    }

    input->primaryPressed = g_bootButton.stablePressed;
    input->primaryShortPress = primaryShortPress;
    input->primaryLongPress = primaryLongPress;
    if(primaryShortPress || primaryLongPress) {
        Serial.printf("[remote][input] boot gpio%d %s\n",
                      static_cast<int>(kBoardPins.primaryButton),
                      primaryLongPress ? "long" : "short");
    }
    pollTouch(input);
    serviceDisplayAutoRotate(input);
    pollPmic(input);
}

RemoteCanvas& lilygo_canvas() {
    return g_canvas;
}

const RemoteBoardInfo& lilygo_info() {
    return kBoardInfo;
}

const RemoteBoardServices& lilygo_services() {
    return g_boardServices;
}

const RemoteBoardPins& lilygo_pins() {
    return kBoardPins;
}

const RemoteBoardFeatureMap& lilygo_feature_map() {
    return g_boardFeatureMap;
}

void lilygo_apply_indicator(const RemoteIndicatorState&) {}

bool lilygo_set_backlight(uint8_t level) {
    if(!g_displayReady) {
        return false;
    }
    g_display->setBrightness(level);
    return true;
}

RemoteBatteryStatus lilygo_battery_status() {
    return cachedPmicBatteryStatus();
}

int32_t lilygo_battery_millivolts() {
    return lilygo_battery_status().millivolts;
}

bool lilygo_input_pending() {
    const bool buttonPollDue = (millis() - g_lastButtonPollAt) >= kButtonPollMs;
    const bool touchPollDue = g_touchReady && ((millis() - g_lastTouchPollAt) >= kTouchIdlePollMs);
    const bool imuPollDue = g_imuReady &&
                            ((millis() - g_displayAutoRotateLastSampleAt) >=
                             remote_config::kDisplayAutoRotateSampleMs);
    return buttonPollDue || g_bootButtonInterruptPending || g_touchInterruptPending ||
           g_touchState.pressed || touchPollDue ||
           (g_touchReady && digitalRead(kBoardPins.touchInt) == LOW) || imuPollDue;
}

const RemoteBoardDriver kDriver = {
    "lilygo-t4-s3-h678",
    lilygo_probe,
    lilygo_init,
    lilygo_poll_input,
    lilygo_canvas,
    lilygo_info,
    lilygo_services,
    lilygo_pins,
    lilygo_feature_map,
    lilygo_apply_indicator,
    lilygo_set_backlight,
    lilygo_battery_status,
    lilygo_battery_millivolts,
    lilygo_input_pending,
};

}  // namespace

const RemoteBoardDriver* remote_board_lilygo_t4_s3_h678_driver() {
    return &kDriver;
}

#endif
