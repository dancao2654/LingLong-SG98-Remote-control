#include "remote_board_driver.h"
#include "remote_runtime.h"

#include <Adafruit_GFX.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrv.hpp>
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace {

constexpr int kDisplayWidth = 466;
constexpr int kDisplayHeight = 466;
constexpr uint8_t kTouchAddress = 0x5A;
constexpr uint8_t kIoExpanderAddress = 0x20;
constexpr uint8_t kIoExpanderInputReg = 0x00;
constexpr uint8_t kIoExpanderConfigReg = 0x03;
constexpr uint8_t kBootButtonPin = 0;
constexpr uint8_t kPmicIrqBit = 5;
constexpr uint32_t kI2cClock = 100000U;
constexpr uint32_t kButtonDebounceMs = 18;
constexpr uint8_t kTouchProbeAttempts = 6;
constexpr uint32_t kTouchProbeRetryMs = 40;
constexpr uint8_t kTouchInitAttempts = 8;
constexpr uint32_t kTouchInitRetryMs = 80;
constexpr uint32_t kTouchResetSettleMs = 1000;
constexpr uint8_t kDisplayInitAttempts = 3;
constexpr uint32_t kDisplayInitRetryMs = 60;
constexpr uint32_t kTouchDebounceMs = 28;
constexpr uint32_t kTouchReportHoldMs = 55;
constexpr uint8_t kTouchMaxPoints = 5;
constexpr uint32_t kTouchIdleProbeMs = 16;
constexpr uint32_t kTouchActiveProbeMs = 12;
constexpr uint32_t kTouchReadFailureBackoffMs = 80;
constexpr uint8_t kTouchReadFailureTripCount = 2;
constexpr uint8_t kTouchPressConfirmSamples = 1;
constexpr uint32_t kTouchReleaseHoldMs = 90;
constexpr uint32_t kPmicSensePollMs = 60;

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
    pins.audioDout = 8;
    pins.audioDin = 10;
    pins.audioBclk = 9;
    pins.audioLrclk = 45;
    pins.audioMclk = 42;
    pins.audioEnable = 46;
    pins.primaryButton = kBootButtonPin;
    return pins;
}();

struct ButtonTracker {
    bool initialized = false;
    bool stablePressed = false;
    bool lastSamplePressed = false;
    uint32_t debounceStartedAt = 0;
};

struct TouchDebounceTracker {
    bool initialized = false;
    bool stablePressed = false;
    bool lastSamplePressed = false;
    uint32_t debounceStartedAt = 0;
    int16_t rawX = 0;
    int16_t rawY = 0;
    int16_t stableX = 0;
    int16_t stableY = 0;
};

struct TouchReportTracker {
    bool active = false;
    uint32_t lastReportAt = 0;
    int16_t x = 0;
    int16_t y = 0;
};

class BufferedWaveshareDisplay : public Arduino_GFX {
  public:
    explicit BufferedWaveshareDisplay(Arduino_GFX* output)
        : Arduino_GFX(kDisplayWidth, kDisplayHeight), output_(output) {}

    bool begin(int32_t speed = GFX_NOT_DEFINED) override {
        (void)speed;
        if(framebuffer_ != nullptr) {
            return true;
        }

        const size_t pixelCount = static_cast<size_t>(kDisplayWidth) * static_cast<size_t>(kDisplayHeight);
        const size_t bufferBytes = pixelCount * sizeof(uint16_t);
        framebuffer_ = static_cast<uint16_t*>(
            heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if(framebuffer_ != nullptr) {
            usingPsram_ = true;
        } else {
            framebuffer_ = static_cast<uint16_t*>(heap_caps_malloc(bufferBytes, MALLOC_CAP_8BIT));
            usingPsram_ = false;
        }

        if(framebuffer_ == nullptr) {
            return false;
        }

        for(size_t i = 0; i < pixelCount; ++i) {
            framebuffer_[i] = 0;
        }
        return true;
    }

    void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
        if(framebuffer_ == nullptr) {
            return;
        }
        framebuffer_[static_cast<size_t>(y) * kDisplayWidth + x] = color;
    }

    void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        if(framebuffer_ == nullptr || w <= 0 || h <= 0) {
            return;
        }

        for(int16_t row = 0; row < h; ++row) {
            uint16_t* line = framebuffer_ + (static_cast<size_t>(y + row) * kDisplayWidth) + x;
            for(int16_t col = 0; col < w; ++col) {
                line[col] = color;
            }
        }
    }

    void flush(bool force_flush = false) override {
        (void)force_flush;
        if(output_ == nullptr || framebuffer_ == nullptr) {
            return;
        }
        output_->draw16bitRGBBitmap(0, 0, framebuffer_, kDisplayWidth, kDisplayHeight);
    }

    bool usingPsram() const {
        return usingPsram_;
    }

  private:
    Arduino_GFX* output_ = nullptr;
    uint16_t* framebuffer_ = nullptr;
    bool usingPsram_ = false;
};

class WaveshareCanvas : public RemoteCanvas {
  public:
    explicit WaveshareCanvas(Arduino_GFX** display) : display_(display) {}

    uint16_t width() const override {
        return kDisplayWidth;
    }

    uint16_t height() const override {
        return kDisplayHeight;
    }

    void fillScreen(uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->fillScreen(color);
        }
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->fillRect(x, y, w, h, color);
        }
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->drawRect(x, y, w, h, color);
        }
    }

    void fillRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->fillRoundRect(x, y, w, h, radius, color);
        }
    }

    void drawRoundRect(int16_t x,
                       int16_t y,
                       int16_t w,
                       int16_t h,
                       int16_t radius,
                       uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->drawRoundRect(x, y, w, h, radius, color);
        }
    }

    void fillCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->fillCircle(x, y, radius, color);
        }
    }

    void drawCircle(int16_t x, int16_t y, int16_t radius, uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->drawCircle(x, y, radius, color);
        }
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->drawLine(x0, y0, x1, y1, color);
        }
    }

    void drawCenteredText(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx == nullptr) {
            return;
        }
        configureText(style, textColor, bgColor);
        int16_t x1 = 0;
        int16_t y1 = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        gfx->setCursor(x - static_cast<int16_t>(w / 2), y - static_cast<int16_t>(h / 2));
        gfx->print(text);
    }

    void drawCenteredTextRotated(const String& text,
                                 int16_t x,
                                 int16_t y,
                                 float angleDegrees,
                                 RemoteTextStyle style,
                                 uint16_t textColor,
                                 uint16_t bgColor) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx == nullptr || text.length() == 0) {
            return;
        }

        configureText(style, textColor, bgColor);
        int16_t x1 = 0;
        int16_t y1 = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        const int16_t spriteW = max<int16_t>(1, static_cast<int16_t>(w + 10));
        const int16_t spriteH = max<int16_t>(1, static_cast<int16_t>(h + 10));

        uint16_t transparent = 0x0001;
        if(transparent == textColor || transparent == bgColor) {
            transparent = 0xF81F;
        }

        GFXcanvas16 glyph(spriteW, spriteH);
        uint16_t* buffer = glyph.getBuffer();
        if(buffer == nullptr) {
            drawCenteredText(text, x, y, style, textColor, bgColor);
            return;
        }

        glyph.setFont(nullptr);
        glyph.setTextSize(sizeForStyle(style));
        glyph.setTextWrap(false);
        glyph.fillScreen(transparent);
        glyph.setTextColor(textColor, transparent);
        glyph.setCursor(static_cast<int16_t>((spriteW - w) / 2) - x1,
                        static_cast<int16_t>((spriteH - h) / 2) - y1);
        glyph.print(text);

        const float radians = angleDegrees * 0.01745329252f;
        const float c = cosf(radians);
        const float s = sinf(radians);
        const float pivotX = (static_cast<float>(spriteW) - 1.0f) * 0.5f;
        const float pivotY = (static_cast<float>(spriteH) - 1.0f) * 0.5f;

        for(int16_t row = 0; row < spriteH; ++row) {
            for(int16_t col = 0; col < spriteW; ++col) {
                const uint16_t color = buffer[(static_cast<size_t>(row) * spriteW) + col];
                if(color == transparent) {
                    continue;
                }

                const float dx = static_cast<float>(col) - pivotX;
                const float dy = static_cast<float>(row) - pivotY;
                const int16_t dstX =
                    static_cast<int16_t>(lroundf(static_cast<float>(x) + (dx * c) - (dy * s)));
                const int16_t dstY =
                    static_cast<int16_t>(lroundf(static_cast<float>(y) + (dx * s) + (dy * c)));
                gfx->drawPixel(dstX, dstY, color);
            }
        }
    }

    void drawTextTopLeft(const String& text,
                         int16_t x,
                         int16_t y,
                         RemoteTextStyle style,
                         uint16_t textColor,
                         uint16_t bgColor) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx == nullptr) {
            return;
        }
        configureText(style, textColor, bgColor);
        gfx->setCursor(x, y + baselineOffset(style));
        gfx->print(text);
    }

    void drawTextTopRight(const String& text,
                          int16_t x,
                          int16_t y,
                          RemoteTextStyle style,
                          uint16_t textColor,
                          uint16_t bgColor) override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx == nullptr) {
            return;
        }
        configureText(style, textColor, bgColor);
        int16_t x1 = 0;
        int16_t y1 = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        gfx->setCursor(x - static_cast<int16_t>(w), y + baselineOffset(style));
        gfx->print(text);
    }

    void present() override {
        Arduino_GFX* gfx = activeDisplay();
        if(gfx != nullptr) {
            gfx->flush();
        }
    }

  private:
    Arduino_GFX* activeDisplay() const {
        if(display_ == nullptr) {
            return nullptr;
        }
        return *display_;
    }

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
        Arduino_GFX* gfx = activeDisplay();
        if(gfx == nullptr) {
            return;
        }
        gfx->setFont(nullptr);
        gfx->setTextSize(sizeForStyle(style));
        gfx->setTextWrap(false);
        gfx->setTextColor(textColor, bgColor);
    }

    Arduino_GFX** display_;
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

BufferedWaveshareDisplay g_bufferedDisplay(g_display);
Arduino_GFX* g_drawDisplay = g_display;
WaveshareCanvas g_canvas(&g_drawDisplay);
TouchDrvCST92xx g_touch;
XPowersAXP2101 g_pmic;
RemoteTouchState g_touchState;
bool g_pmicReady = false;
bool g_ioExpanderReady = false;
ButtonTracker g_bootButton;
TouchDebounceTracker g_touchDebounce;
TouchReportTracker g_touchReport;
volatile bool g_bootButtonInterruptPending = true;
volatile bool g_touchInterruptPending = true;
uint8_t g_touchReadFailureCount = 0;
uint32_t g_touchBackoffUntil = 0;
uint32_t g_touchLastProbeAt = 0;
volatile uint32_t g_touchIrqCount = 0;
uint32_t g_touchSuccessCount = 0;
bool g_touchStablePressed = false;
uint8_t g_touchPressSampleCount = 0;
uint8_t g_touchReleaseSampleCount = 0;
int16_t g_touchStableX = 0;
int16_t g_touchStableY = 0;
uint32_t g_touchLastGoodSampleAt = 0;
uint32_t g_pmicLastSenseAt = 0;
bool g_pmicIrqLatched = false;

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
        static_cast<uint32_t>(RemoteWakeSource::Touch) |
            static_cast<uint32_t>(RemoteWakeSource::PrimaryButton),
    },
    {
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
    },
    {
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Mapped,
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
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Implemented,
        true,
        true,
    },
};

bool readIoExpanderRegister(uint8_t reg, uint8_t* value) {
    if(value == nullptr) {
        return false;
    }

    Wire.beginTransmission(kIoExpanderAddress);
    Wire.write(reg);
    if(Wire.endTransmission(false) != 0) {
        return false;
    }

    if(Wire.requestFrom(static_cast<int>(kIoExpanderAddress), 1) != 1) {
        return false;
    }

    *value = Wire.read();
    return true;
}

bool writeIoExpanderRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(kIoExpanderAddress);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool initIoExpander() {
    uint8_t config = 0xFF;
    if(!readIoExpanderRegister(kIoExpanderConfigReg, &config)) {
        Serial.println("[remote] waveshare io expander config read failed");
        return false;
    }

    // The Waveshare schematic routes PMIC IRQ to EXIO5. Keep all expander pins
    // configured as inputs so we can poll interrupt/status lines safely.
    if(config != 0xFF && !writeIoExpanderRegister(kIoExpanderConfigReg, 0xFF)) {
        Serial.println("[remote] waveshare io expander config write failed");
        return false;
    }

    Serial.println("[remote] waveshare io expander ready");
    return true;
}

void initButtonTracker(ButtonTracker* tracker, bool pressed) {
    if(tracker == nullptr) {
        return;
    }

    tracker->initialized = true;
    tracker->stablePressed = pressed;
    tracker->lastSamplePressed = pressed;
    tracker->debounceStartedAt = millis();
}

void updateButtonTracker(ButtonTracker* tracker, bool pressed, bool* justPressed, bool* justReleased) {
    if(justPressed != nullptr) {
        *justPressed = false;
    }
    if(justReleased != nullptr) {
        *justReleased = false;
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
        return;
    }

    tracker->stablePressed = pressed;
    if(pressed) {
        if(justPressed != nullptr) {
            *justPressed = true;
        }
    } else {
        if(justReleased != nullptr) {
            *justReleased = true;
        }
    }
}

bool readBootButtonPressed(bool* pressed) {
    if(pressed == nullptr) {
        return false;
    }

    *pressed = digitalRead(kBootButtonPin) == LOW;
    return true;
}

bool readTouchPointSample(int16_t* x, int16_t* y) {
    if(x == nullptr || y == nullptr) {
        return false;
    }

    const TouchPoints& points = g_touch.getTouchPoints();
    if(!points.hasPoints()) {
        return false;
    }

    const TouchPoint& point = points[0];
    if(point.x < 0 || point.x >= kDisplayWidth || point.y < 0 || point.y >= kDisplayHeight) {
        return false;
    }

    *x = static_cast<int16_t>(point.x);
    *y = static_cast<int16_t>(point.y);
    return true;
}

void IRAM_ATTR waveshareBootButtonIsr() {
    g_bootButtonInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

void IRAM_ATTR waveshareTouchIsr() {
    ++g_touchIrqCount;
    g_touchInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

bool readPmicIrqActive(bool* active) {
    if(active == nullptr || !g_ioExpanderReady) {
        return false;
    }

    uint8_t inputs = 0;
    if(!readIoExpanderRegister(kIoExpanderInputReg, &inputs)) {
        return false;
    }

    // EXIO5 is wired to the AXP2101 IRQ line and is active-low.
    *active = (inputs & static_cast<uint8_t>(1u << kPmicIrqBit)) == 0;
    return true;
}

void updatePmicButton(RemoteInputState* input, bool deferForTouch) {
    if(input == nullptr || !g_pmicReady) {
        return;
    }

    const uint32_t now = millis();
    if(!g_pmicIrqLatched) {
        if(deferForTouch) {
            return;
        }
        if((now - g_pmicLastSenseAt) < kPmicSensePollMs) {
            return;
        }
        g_pmicLastSenseAt = now;

        bool irqActive = false;
        if(!readPmicIrqActive(&irqActive) || !irqActive) {
            return;
        }
        g_pmicIrqLatched = true;
    }

    g_pmic.getIrqStatus();
    const bool shortPress = g_pmic.isPekeyShortPressIrq();
    const bool longPress = g_pmic.isPekeyLongPressIrq();
    g_pmic.clearIrqStatus();
    g_pmicIrqLatched = false;

    input->secondaryPressed = false;
    if(longPress) {
        input->secondaryLongPress = true;
    }
    if(shortPress) {
        input->secondaryShortPress = true;
    }
}

void initTouchDebounceTracker(TouchDebounceTracker* tracker, bool pressed, int16_t x, int16_t y) {
    if(tracker == nullptr) {
        return;
    }

    tracker->initialized = true;
    tracker->stablePressed = pressed;
    tracker->lastSamplePressed = pressed;
    tracker->debounceStartedAt = millis();
    tracker->rawX = x;
    tracker->rawY = y;
    tracker->stableX = x;
    tracker->stableY = y;
}

void updateTouchDebounceTracker(TouchDebounceTracker* tracker,
                                bool pressed,
                                int16_t x,
                                int16_t y,
                                bool* justPressed,
                                bool* justReleased) {
    if(justPressed != nullptr) {
        *justPressed = false;
    }
    if(justReleased != nullptr) {
        *justReleased = false;
    }
    if(tracker == nullptr) {
        return;
    }

    const uint32_t now = millis();
    if(!tracker->initialized) {
        initTouchDebounceTracker(tracker, pressed, x, y);
        return;
    }

    if(pressed) {
        tracker->rawX = x;
        tracker->rawY = y;
    }

    if(pressed != tracker->lastSamplePressed) {
        tracker->lastSamplePressed = pressed;
        tracker->debounceStartedAt = now;
    }

    if(now - tracker->debounceStartedAt < kTouchDebounceMs) {
        return;
    }

    if(pressed == tracker->stablePressed) {
        if(pressed) {
            tracker->stableX = tracker->rawX;
            tracker->stableY = tracker->rawY;
        }
        return;
    }

    tracker->stablePressed = pressed;
    if(pressed) {
        tracker->stableX = tracker->rawX;
        tracker->stableY = tracker->rawY;
        if(justPressed != nullptr) {
            *justPressed = true;
        }
    } else if(justReleased != nullptr) {
        *justReleased = true;
    }
}

bool pingTouchController() {
    pinMode(kBoardPins.touchReset, OUTPUT);
    pinMode(kBootButtonPin, INPUT_PULLUP);
    pinMode(kBoardPins.touchInt, INPUT_PULLUP);
    digitalWrite(kBoardPins.touchReset, LOW);
    delay(30);
    digitalWrite(kBoardPins.touchReset, HIGH);
    delay(kTouchResetSettleMs);
    Wire.begin(kBoardPins.touchSda, kBoardPins.touchScl, kI2cClock);
    for(uint8_t attempt = 1; attempt <= kTouchProbeAttempts; ++attempt) {
        Wire.beginTransmission(kTouchAddress);
        if(Wire.endTransmission() == 0) {
            if(attempt > 1) {
                Serial.printf("[remote] waveshare probe touch ready after %u attempts\n", attempt);
            }
            return true;
        }
        delay(kTouchProbeRetryMs);
    }
    Serial.println("[remote] waveshare probe touch controller not detected");
    return false;
}

}  // namespace

bool waveshare_probe() {
    return pingTouchController();
}

bool waveshare_init() {
    Serial.println("[remote] waveshare init start");
    Wire.begin(kBoardPins.touchSda, kBoardPins.touchScl, kI2cClock);
    pinMode(kBootButtonPin, INPUT_PULLUP);

    pinMode(kBoardPins.touchReset, OUTPUT);
    pinMode(kBoardPins.touchInt, INPUT_PULLUP);
    digitalWrite(kBoardPins.touchReset, LOW);
    delay(30);
    digitalWrite(kBoardPins.touchReset, HIGH);
    delay(50);
    delay(kTouchResetSettleMs);

    g_touch.setPins(kBoardPins.touchReset, kBoardPins.touchInt);
    bool touchReady = false;
    for(uint8_t attempt = 1; attempt <= kTouchInitAttempts; ++attempt) {
        if(g_touch.begin(Wire, kTouchAddress, kBoardPins.touchSda, kBoardPins.touchScl)) {
            touchReady = true;
            if(attempt > 1) {
                Serial.printf("[remote] waveshare touch init succeeded on attempt %u\n", attempt);
            }
            break;
        }
        Serial.printf("[remote] waveshare touch init failed on attempt %u/%u\n",
                      attempt,
                      kTouchInitAttempts);
        digitalWrite(kBoardPins.touchReset, LOW);
        delay(20);
        digitalWrite(kBoardPins.touchReset, HIGH);
        delay(kTouchInitRetryMs);
    }
    if(!touchReady) {
        Serial.println("[remote] waveshare touch init gave up");
        return false;
    }
    g_touch.reset();
    delay(120);
    pinMode(kBoardPins.touchInt, INPUT_PULLUP);
    g_touch.setMaxCoordinates(kDisplayWidth, kDisplayHeight);
    g_touch.setMirrorXY(true, true);
    Serial.printf("[remote] waveshare touch ready int=%d\n",
                  digitalRead(kBoardPins.touchInt) == LOW ? 0 : 1);

    g_ioExpanderReady = initIoExpander();
    if(!g_ioExpanderReady) {
        Serial.println("[remote] waveshare power button sense unavailable");
    }

    g_pmicReady = g_pmic.begin(Wire, AXP2101_SLAVE_ADDRESS, kBoardPins.touchSda, kBoardPins.touchScl);
    if(g_pmicReady) {
        g_pmic.enableBattDetection();
        g_pmic.enableBattVoltageMeasure();
        g_pmic.enableVbusVoltageMeasure();
        g_pmic.enableSystemVoltageMeasure();
        g_pmic.clearIrqStatus();
        g_pmic.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
        Serial.println("[remote] waveshare pmic ready");
    } else {
        Serial.println("[remote] waveshare pmic init failed");
    }

    bool displayReady = false;
    for(uint8_t attempt = 1; attempt <= kDisplayInitAttempts; ++attempt) {
        if(g_display->begin()) {
            displayReady = true;
            if(attempt > 1) {
                Serial.printf("[remote] waveshare display init succeeded on attempt %u\n", attempt);
            }
            break;
        }
        Serial.printf("[remote] waveshare display init failed on attempt %u/%u\n",
                      attempt,
                      kDisplayInitAttempts);
        delay(kDisplayInitRetryMs);
    }
    if(!displayReady) {
        Serial.println("[remote] waveshare display init gave up");
        return false;
    }

    g_drawDisplay = g_display;
    g_display->setRotation(0);
    g_display->setBrightness(200);
    if(g_bufferedDisplay.begin()) {
        g_bufferedDisplay.setRotation(0);
        g_drawDisplay = &g_bufferedDisplay;
        g_drawDisplay->fillScreen(0x0000);
        g_drawDisplay->flush(true);
        Serial.printf("[remote] waveshare framebuffer enabled (%s)\n",
                      g_bufferedDisplay.usingPsram() ? "psram" : "internal");
    } else {
        g_display->fillScreen(0x0000);
        Serial.println("[remote] waveshare framebuffer unavailable, drawing direct");
    }
    g_touchState = {};
    {
        bool bootPressed = false;
        if(readBootButtonPressed(&bootPressed)) {
            initButtonTracker(&g_bootButton, bootPressed);
        }
    }
    g_bootButtonInterruptPending = true;
    g_touchInterruptPending = true;
    g_touchReadFailureCount = 0;
    g_touchBackoffUntil = 0;
    g_touchLastProbeAt = 0;
    g_touchIrqCount = 0;
    g_touchSuccessCount = 0;
    g_touchStablePressed = false;
    g_touchPressSampleCount = 0;
    g_touchReleaseSampleCount = 0;
    g_touchStableX = 0;
    g_touchStableY = 0;
    g_touchLastGoodSampleAt = 0;
    g_pmicLastSenseAt = 0;
    g_pmicIrqLatched = false;
    attachInterrupt(kBootButtonPin, waveshareBootButtonIsr, CHANGE);
    attachInterrupt(kBoardPins.touchInt, waveshareTouchIsr, FALLING);
    Serial.println("[remote] waveshare init complete");
    g_touchDebounce = {};
    g_touchReport = {};
    return true;
}

void waveshare_poll_input(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    *input = {};

    bool bootPressed = g_bootButton.stablePressed;
    bool bootJustPressed = false;
    bool bootJustReleased = false;
    if(g_bootButtonInterruptPending || !g_bootButton.initialized ||
       g_bootButton.lastSamplePressed != g_bootButton.stablePressed) {
        if(readBootButtonPressed(&bootPressed)) {
            updateButtonTracker(&g_bootButton, bootPressed, &bootJustPressed, &bootJustReleased);
        }
    } else {
        bootPressed = g_bootButton.stablePressed;
    }

    if(g_bootButton.initialized &&
       g_bootButton.lastSamplePressed == g_bootButton.stablePressed &&
       bootPressed == g_bootButton.stablePressed) {
        g_bootButtonInterruptPending = false;
    }

    input->primaryPressed = g_bootButton.stablePressed;
    input->primaryShortPress = bootJustReleased;
    input->primaryLongPress = false;

    const uint32_t now = millis();
    bool rawTouched = false;
    bool sampledTouch = false;
    int16_t touchX = g_touchStablePressed ? g_touchStableX : g_touchState.x;
    int16_t touchY = g_touchStablePressed ? g_touchStableY : g_touchState.y;
    const bool touchIrqActive = digitalRead(kBoardPins.touchInt) == LOW;
    const bool touchNeedsSample = g_touchInterruptPending || touchIrqActive || g_touchStablePressed;
    updatePmicButton(input, touchNeedsSample);
    if(touchNeedsSample) {
        const uint32_t probeIntervalMs = g_touchStablePressed ? kTouchActiveProbeMs : kTouchIdleProbeMs;
        const bool probeDue = g_touchLastProbeAt == 0 ||
                              static_cast<uint32_t>(now - g_touchLastProbeAt) >= probeIntervalMs;
        const bool inBackoff = !g_touchStablePressed && now < g_touchBackoffUntil;
        if(probeDue && !inBackoff) {
            g_touchLastProbeAt = now;
            sampledTouch = true;
            rawTouched = readTouchPointSample(&touchX, &touchY);
            if(rawTouched) {
                ++g_touchSuccessCount;
                g_touchReadFailureCount = 0;
                g_touchBackoffUntil = 0;
            } else if(!touchIrqActive) {
                g_touchReadFailureCount = 0;
                g_touchBackoffUntil = 0;
            } else if(!g_touchStablePressed && g_touchReadFailureCount < 0xFF) {
                ++g_touchReadFailureCount;
                if(g_touchReadFailureCount >= kTouchReadFailureTripCount) {
                    g_touchBackoffUntil = now + kTouchReadFailureBackoffMs;
                }
            }

            if(rawTouched || !touchIrqActive) {
                g_touchInterruptPending = false;
            }
        }
    }

    bool touchJustPressed = false;
    bool touchJustReleased = false;
    if(rawTouched) {
        g_touchLastGoodSampleAt = now;
        g_touchReleaseSampleCount = 0;
        if(g_touchPressSampleCount < 0xFF) {
            ++g_touchPressSampleCount;
        }
        if(g_touchStablePressed || g_touchPressSampleCount >= kTouchPressConfirmSamples) {
            touchJustPressed = !g_touchStablePressed;
            g_touchStablePressed = true;
            g_touchStableX = touchX;
            g_touchStableY = touchY;
        }
    } else {
        g_touchPressSampleCount = 0;
        if(g_touchStablePressed && sampledTouch &&
           static_cast<uint32_t>(now - g_touchLastGoodSampleAt) <= kTouchReleaseHoldMs) {
            touchX = g_touchStableX;
            touchY = g_touchStableY;
        } else if(g_touchStablePressed && sampledTouch) {
            g_touchStablePressed = false;
            g_touchReleaseSampleCount = 0;
            touchJustReleased = true;
        } else if(!g_touchStablePressed) {
            g_touchReleaseSampleCount = 0;
        }
    }

    input->touch.justPressed = touchJustPressed;
    input->touch.justReleased = touchJustReleased;
    input->touch.pressed = g_touchStablePressed;
    input->touch.x = g_touchStablePressed ? g_touchStableX : g_touchState.x;
    input->touch.y = g_touchStablePressed ? g_touchStableY : g_touchState.y;

    g_touchState = input->touch;
}

bool waveshare_input_pending() {
    return g_bootButtonInterruptPending ||
           (g_bootButton.initialized && g_bootButton.lastSamplePressed != g_bootButton.stablePressed) ||
           g_touchInterruptPending || g_touchStablePressed;
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
    waveshare_input_pending,
};

const RemoteBoardDriver* remote_board_waveshare_amoled175_driver() {
    return &kWaveshareDriver;
}
