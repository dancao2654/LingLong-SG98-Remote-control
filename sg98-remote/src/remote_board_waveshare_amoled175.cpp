#include "remote_board_driver.h"
#include "remote_config.h"
#include "remote_runtime.h"

#include <Adafruit_GFX.h>
#include <Arduino_GFX_Library.h>
#include <SensorQMI8658.hpp>
#include <TouchDrv.hpp>
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>

namespace {

constexpr int kDisplayWidth = 466;
constexpr int kDisplayHeight = 466;
constexpr uint8_t kTouchAddress = 0x5A;
constexpr uint8_t kRtcAddress = 0x51;
constexpr uint8_t kAudioCodecAddress = 0x18;
constexpr uint8_t kIoExpanderAddress = 0x20;
constexpr uint8_t kIoExpanderInputReg = 0x00;
constexpr uint8_t kIoExpanderConfigReg = 0x03;
constexpr uint8_t kBootButtonPin = 0;
constexpr uint8_t kPmicIrqBit = 5;
constexpr uint8_t kPmicAddress = AXP2101_SLAVE_ADDRESS;
constexpr uint8_t kPmicChipIdReg = XPOWERS_AXP2101_IC_TYPE;
constexpr uint8_t kPmicChipId = XPOWERS_AXP2101_CHIP_ID;
constexpr uint8_t kPmicIntEnable2Reg = XPOWERS_AXP2101_INTEN2;
constexpr uint8_t kPmicIntEnable3Reg = XPOWERS_AXP2101_INTEN3;
constexpr uint8_t kPmicIntStatus1Reg = XPOWERS_AXP2101_INTSTS1;
constexpr uint8_t kPmicIntStatus2Reg = XPOWERS_AXP2101_INTSTS2;
constexpr uint8_t kPmicIntStatus3Reg = XPOWERS_AXP2101_INTSTS3;
constexpr uint8_t kPmicPkeyLongMask = static_cast<uint8_t>(XPOWERS_AXP2101_PKEY_LONG_IRQ >> 8);
constexpr uint8_t kPmicPkeyShortMask = static_cast<uint8_t>(XPOWERS_AXP2101_PKEY_SHORT_IRQ >> 8);
constexpr uint8_t kPmicBatRemoveMask = static_cast<uint8_t>(XPOWERS_AXP2101_BAT_REMOVE_IRQ >> 8);
constexpr uint8_t kPmicBatInsertMask = static_cast<uint8_t>(XPOWERS_AXP2101_BAT_INSERT_IRQ >> 8);
constexpr uint8_t kPmicVbusRemoveMask = static_cast<uint8_t>(XPOWERS_AXP2101_VBUS_REMOVE_IRQ >> 8);
constexpr uint8_t kPmicVbusInsertMask = static_cast<uint8_t>(XPOWERS_AXP2101_VBUS_INSERT_IRQ >> 8);
constexpr uint8_t kPmicChargeStartMask = static_cast<uint8_t>(XPOWERS_AXP2101_BAT_CHG_START_IRQ >> 16);
constexpr uint8_t kPmicChargeDoneMask = static_cast<uint8_t>(XPOWERS_AXP2101_BAT_CHG_DONE_IRQ >> 16);
constexpr uint8_t kPmicPowerStatus2Mask = static_cast<uint8_t>(kPmicBatRemoveMask |
                                                              kPmicBatInsertMask |
                                                              kPmicVbusRemoveMask |
                                                              kPmicVbusInsertMask);
constexpr uint8_t kPmicPowerStatus3Mask = static_cast<uint8_t>(kPmicChargeStartMask |
                                                              kPmicChargeDoneMask);
constexpr uint8_t kPmicStatus1Reg = XPOWERS_AXP2101_STATUS1;
constexpr uint8_t kPmicStatus2Reg = XPOWERS_AXP2101_STATUS2;
constexpr uint8_t kPmicAdcCtrlReg = XPOWERS_AXP2101_ADC_CHANNEL_CTRL;
constexpr uint8_t kPmicBatteryDetectReg = XPOWERS_AXP2101_BAT_DET_CTRL;
constexpr uint8_t kPmicBatteryPercentReg = XPOWERS_AXP2101_BAT_PERCENT_DATA;
constexpr uint8_t kPmicBattVoltageHighReg = XPOWERS_AXP2101_ADC_DATA_RELUST0;
constexpr uint8_t kPmicBattVoltageLowReg = XPOWERS_AXP2101_ADC_DATA_RELUST1;
constexpr uint8_t kPmicVbusVoltageHighReg = XPOWERS_AXP2101_ADC_DATA_RELUST4;
constexpr uint8_t kPmicVbusVoltageLowReg = XPOWERS_AXP2101_ADC_DATA_RELUST5;
constexpr uint8_t kPmicSystemVoltageHighReg = XPOWERS_AXP2101_ADC_DATA_RELUST6;
constexpr uint8_t kPmicSystemVoltageLowReg = XPOWERS_AXP2101_ADC_DATA_RELUST7;
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
constexpr uint32_t kTouchIdleProbeMs = 8;
constexpr uint32_t kTouchActiveProbeMs = 8;
constexpr uint32_t kTouchReadFailureBackoffMs = 30;
constexpr uint8_t kTouchReadFailureTripCount = 4;
constexpr uint8_t kTouchPressConfirmSamples = 1;
constexpr uint32_t kTouchReleaseHoldMs = 90;
constexpr uint32_t kPmicSensePollMs = 60;
constexpr uint8_t kImuAddress = QMI8658_H_SLAVE_ADDRESS;
constexpr uint8_t kImuWakeThresholdMg = 80;
constexpr SensorQMI8658::AccelODR kImuWakeOdr = SensorQMI8658::ACC_ODR_LOWPOWER_11Hz;
constexpr float kImuWakeOdrLabelHz = 11.0f;
constexpr uint32_t kBaseWakeSources = static_cast<uint32_t>(RemoteWakeSource::Touch) |
                                      static_cast<uint32_t>(RemoteWakeSource::PrimaryButton);

struct WavesharePinProfile {
    const char* key = "";
    const char* label = "";
    RemoteBoardPins pins = {};
};

RemoteBoardPins makeStockPins() {
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
    pins.imuInt = 21;
    pins.audioDout = 8;
    pins.audioDin = 10;
    pins.audioBclk = 9;
    pins.audioLrclk = 45;
    pins.audioMclk = 42;
    pins.audioEnable = 46;
    pins.primaryButton = kBootButtonPin;
    return pins;
}

RemoteBoardPins makeAltPins4145() {
    RemoteBoardPins pins = {};
    pins.displayData0 = 12;
    pins.displayData1 = 8;
    pins.displayData2 = 14;
    pins.displayData3 = 9;
    pins.displaySclk = 13;
    pins.displayCs = 7;
    pins.displayReset = 11;
    pins.touchSda = 41;
    pins.touchScl = 45;
    pins.touchInt = 42;
    pins.touchReset = 46;
    pins.imuInt = 38;
    pins.primaryButton = kBootButtonPin;
    return pins;
}

RemoteBoardServices makeBoardServices(const RemoteBoardPins& pins) {
    return {
        &Wire,
        nullptr,
        nullptr,
        &Serial,
        pins.touchSda,
        pins.touchScl,
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
}

const WavesharePinProfile kPinProfiles[] = {
    {"waveshare-stock", "Waveshare stock 1.75", makeStockPins()},
    {"amoled-alt-4145", "Alt 1.75 AMOLED variant", makeAltPins4145()},
};

const WavesharePinProfile* g_activeProfile = &kPinProfiles[0];
RemoteBoardPins g_boardPins = makeStockPins();
RemoteBoardServices g_boardServices = makeBoardServices(g_boardPins);
bool g_profileDetected = false;
#if defined(REMOTE_WAVESHARE_I2C_DIAG)
bool g_loggedDiagnosticI2cPairs = false;
#endif

void resetDisplayObjects();
bool probeI2cDevice(uint8_t address);

void selectProfile(const WavesharePinProfile& profile, bool detected) {
    if(g_activeProfile != &profile) {
        resetDisplayObjects();
    }

    g_activeProfile = &profile;
    g_boardPins = profile.pins;
    g_boardServices = makeBoardServices(g_boardPins);
    g_profileDetected = detected;
}

const char* activeProfileKey() {
    return g_activeProfile != nullptr ? g_activeProfile->key : "unknown";
}

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
    BufferedWaveshareDisplay() : Arduino_GFX(kDisplayWidth, kDisplayHeight) {}

    void setOutput(Arduino_GFX* output) {
        output_ = output;
    }

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

Arduino_ESP32QSPI* g_bus = nullptr;
Arduino_CO5300* g_display = nullptr;
BufferedWaveshareDisplay g_bufferedDisplay;
Arduino_GFX* g_drawDisplay = nullptr;
WaveshareCanvas g_canvas(&g_drawDisplay);
TouchDrvCST92xx g_touch;
XPowersAXP2101 g_pmic;
SensorQMI8658 g_imu;
RemoteTouchState g_touchState;
bool g_pmicReady = false;
bool g_pmicButtonReady = false;
bool g_ioExpanderReady = false;
bool g_imuReady = false;
ButtonTracker g_bootButton;
TouchDebounceTracker g_touchDebounce;
TouchReportTracker g_touchReport;
volatile bool g_bootButtonInterruptPending = true;
volatile bool g_touchInterruptPending = true;
volatile bool g_imuInterruptPending = false;
uint8_t g_touchReadFailureCount = 0;
uint32_t g_touchBackoffUntil = 0;
uint32_t g_touchLastProbeAt = 0;
uint32_t g_touchSuccessCount = 0;
bool g_touchStablePressed = false;
uint8_t g_touchPressSampleCount = 0;
uint8_t g_touchReleaseSampleCount = 0;
int16_t g_touchStableX = 0;
int16_t g_touchStableY = 0;
uint32_t g_touchLastGoodSampleAt = 0;
uint32_t g_pmicLastSenseAt = 0;
uint32_t g_pmicLastBatteryLevelAt = 0;
uint32_t g_imuLastPollAt = 0;
uint8_t g_i2cRecoveryLogCount = 0;
bool g_pmicIrqLatched = false;
bool g_motionActivityPending = false;
RemoteBatteryStatus g_pmicBatteryCache = {};

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

RemoteBoardFeatureMap g_boardFeatureMap = {
    {
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Implemented,
        RemoteBoardSupportLevel::Mapped,
        RemoteBoardSupportLevel::Present,
        kBaseWakeSources,
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

void resetDisplayObjects() {
    g_drawDisplay = nullptr;
    g_bufferedDisplay.setOutput(nullptr);

    if(g_display != nullptr) {
        delete g_display;
        g_display = nullptr;
    }
    if(g_bus != nullptr) {
        delete g_bus;
        g_bus = nullptr;
    }
}

bool ensureDisplayObjects() {
    if(g_display != nullptr) {
        return true;
    }

    g_bus = new(std::nothrow) Arduino_ESP32QSPI(
        g_boardPins.displayCs,
        g_boardPins.displaySclk,
        g_boardPins.displayData0,
        g_boardPins.displayData1,
        g_boardPins.displayData2,
        g_boardPins.displayData3);
    if(g_bus == nullptr) {
        Serial.println("[remote] waveshare display bus alloc failed");
        return false;
    }

    g_display = new(std::nothrow) Arduino_CO5300(
        g_bus,
        g_boardPins.displayReset,
        0,
        kDisplayWidth,
        kDisplayHeight,
        6,
        0,
        0,
        0);
    if(g_display == nullptr) {
        Serial.println("[remote] waveshare display alloc failed");
        delete g_bus;
        g_bus = nullptr;
        return false;
    }

    g_bufferedDisplay.setOutput(g_display);
    g_drawDisplay = g_display;
    return true;
}

bool beginSharedI2cBus(int8_t sda, int8_t scl) {
    Wire.end();
    delay(2);
    return Wire.begin(sda, scl, kI2cClock);
}

bool recoverSharedI2cBus(const char* reason) {
    Wire.end();
    delay(2);
    const bool ok = Wire.begin(g_boardPins.touchSda, g_boardPins.touchScl, kI2cClock);
    if(g_i2cRecoveryLogCount < 6) {
        Serial.printf("[remote] waveshare i2c recover reason=%s ok=%d\n",
                      reason != nullptr ? reason : "unknown",
                      ok ? 1 : 0);
        ++g_i2cRecoveryLogCount;
    }
    return ok;
}

bool readI2cRegisterOnce(uint8_t address, uint8_t reg, uint8_t* value) {
    if(value == nullptr) {
        return false;
    }

    Wire.beginTransmission(address);
    Wire.write(reg);
    if(Wire.endTransmission(false) != 0) {
        return false;
    }

    if(Wire.requestFrom(static_cast<int>(address), 1) != 1) {
        return false;
    }

    *value = Wire.read();
    return true;
}

bool readI2cRegister(uint8_t address, uint8_t reg, uint8_t* value) {
    if(readI2cRegisterOnce(address, reg, value)) {
        return true;
    }
    if(!recoverSharedI2cBus("read")) {
        return false;
    }
    return readI2cRegisterOnce(address, reg, value);
}

bool writeI2cRegisterOnce(uint8_t address, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool writeI2cRegister(uint8_t address, uint8_t reg, uint8_t value) {
    if(writeI2cRegisterOnce(address, reg, value)) {
        return true;
    }
    if(!recoverSharedI2cBus("write")) {
        return false;
    }
    return writeI2cRegisterOnce(address, reg, value);
}

bool updateI2cRegisterBits(uint8_t address, uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = 0;
    if(!readI2cRegister(address, reg, &current)) {
        return false;
    }

    const uint8_t next = static_cast<uint8_t>((current & ~mask) | (value & mask));
    return next == current || writeI2cRegister(address, reg, next);
}

bool readPmicH6L8(uint8_t highReg, uint8_t lowReg, uint16_t* value) {
    if(value == nullptr) {
        return false;
    }

    uint8_t high = 0;
    uint8_t low = 0;
    if(!readI2cRegister(kPmicAddress, highReg, &high) ||
       !readI2cRegister(kPmicAddress, lowReg, &low)) {
        return false;
    }

    *value = static_cast<uint16_t>(((high & 0x3F) << 8) | low);
    return true;
}

bool readPmicH5L8(uint8_t highReg, uint8_t lowReg, uint16_t* value) {
    if(value == nullptr) {
        return false;
    }

    uint8_t high = 0;
    uint8_t low = 0;
    if(!readI2cRegister(kPmicAddress, highReg, &high) ||
       !readI2cRegister(kPmicAddress, lowReg, &low)) {
        return false;
    }

    *value = static_cast<uint16_t>(((high & 0x1F) << 8) | low);
    return true;
}

bool readIoExpanderRegister(uint8_t reg, uint8_t* value) {
    return readI2cRegister(kIoExpanderAddress, reg, value);
}

bool writeIoExpanderRegister(uint8_t reg, uint8_t value) {
    return writeI2cRegister(kIoExpanderAddress, reg, value);
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
    g_touchInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

void IRAM_ATTR waveshareImuIsr() {
    g_imuInterruptPending = true;
    remote_runtime_notify_input_from_isr();
}

void waveshareImuWakeOnMotion() {
    g_motionActivityPending = true;
}

void setImuWakeSupport(bool enabled) {
    if(enabled) {
        g_boardFeatureMap.motion.imu = RemoteBoardSupportLevel::Implemented;
        g_boardFeatureMap.power.wakeSources =
            kBaseWakeSources | static_cast<uint32_t>(RemoteWakeSource::Motion);
        return;
    }

    g_boardFeatureMap.motion.imu = RemoteBoardSupportLevel::Mapped;
    g_boardFeatureMap.power.wakeSources = kBaseWakeSources;
}

bool initImuMotion() {
    if(!beginSharedI2cBus(g_boardPins.touchSda, g_boardPins.touchScl)) {
        Serial.println("[remote] waveshare imu bus reinit failed");
        return false;
    }

    const bool imuHighReady = probeI2cDevice(QMI8658_H_SLAVE_ADDRESS);
    const bool imuLowReady = probeI2cDevice(QMI8658_L_SLAVE_ADDRESS);
    const uint8_t imuAddress = imuHighReady ? QMI8658_H_SLAVE_ADDRESS
                                            : (imuLowReady ? QMI8658_L_SLAVE_ADDRESS : kImuAddress);
    Serial.printf("[remote] waveshare imu probe 0x%02X=%d 0x%02X=%d selected=0x%02X\n",
                  QMI8658_H_SLAVE_ADDRESS,
                  imuHighReady ? 1 : 0,
                  QMI8658_L_SLAVE_ADDRESS,
                  imuLowReady ? 1 : 0,
                  imuAddress);

    Wire.end();
    delay(2);

    pinMode(g_boardPins.imuInt, INPUT_PULLUP);
    if(!g_imu.begin(Wire, imuAddress, g_boardPins.touchSda, g_boardPins.touchScl)) {
        Serial.println("[remote] waveshare imu init failed");
        return false;
    }

    g_imu.setPins(g_boardPins.imuInt);
    g_imu.setWakeupMotionEventCallBack(waveshareImuWakeOnMotion);
    if(!g_imu.configWakeOnMotion(
           kImuWakeThresholdMg, kImuWakeOdr, SensorQMI8658::INTERRUPT_PIN_2)) {
        Serial.println("[remote] waveshare imu wake-on-motion config failed");
        return false;
    }

    Serial.printf("[remote] waveshare imu ready irq=%d thr=%u odr=%.1fHz\n",
                  g_boardPins.imuInt,
                  kImuWakeThresholdMg,
                  kImuWakeOdrLabelHz);
    return true;
}

void clearPmicButtonIrqStatus() {
    writeI2cRegister(kPmicAddress, kPmicIntStatus1Reg, 0xFF);
    writeI2cRegister(kPmicAddress, kPmicIntStatus2Reg, 0xFF);
    writeI2cRegister(kPmicAddress, kPmicIntStatus3Reg, 0xFF);
}

void initPmicButtonIrq() {
    g_pmicReady = false;
    g_pmicButtonReady = false;

    uint8_t chipId = 0;
    if(!readI2cRegister(kPmicAddress, kPmicChipIdReg, &chipId)) {
        Serial.println("[remote] waveshare pmic button init failed");
        return;
    }
    if(chipId != kPmicChipId) {
        Serial.printf("[remote] waveshare pmic unexpected chip=0x%02X\n", chipId);
        return;
    }

    g_pmicReady =
        updateI2cRegisterBits(kPmicAddress, kPmicBatteryDetectReg, 0x01, 0x01) &&
        updateI2cRegisterBits(kPmicAddress, kPmicAdcCtrlReg, 0x0D, 0x0D);
    if(!g_pmicReady) {
        Serial.println("[remote] waveshare pmic telemetry init failed");
    }

    clearPmicButtonIrqStatus();

    uint8_t irqEnable2 = 0;
    if(!readI2cRegister(kPmicAddress, kPmicIntEnable2Reg, &irqEnable2)) {
        Serial.println("[remote] waveshare pmic irq enable read failed");
        return;
    }
    irqEnable2 |= static_cast<uint8_t>(kPmicPkeyShortMask | kPmicPkeyLongMask);
    irqEnable2 |= kPmicPowerStatus2Mask;
    if(!writeI2cRegister(kPmicAddress, kPmicIntEnable2Reg, irqEnable2)) {
        Serial.println("[remote] waveshare pmic irq enable write failed");
        return;
    }

    bool powerStatusIrqReady = false;
    uint8_t irqEnable3 = 0;
    if(readI2cRegister(kPmicAddress, kPmicIntEnable3Reg, &irqEnable3)) {
        irqEnable3 |= kPmicPowerStatus3Mask;
        powerStatusIrqReady = writeI2cRegister(kPmicAddress, kPmicIntEnable3Reg, irqEnable3);
    }
    if(!powerStatusIrqReady) {
        Serial.println("[remote] waveshare pmic charge irq enable failed");
    }

    g_pmicButtonReady = true;
    Serial.printf("[remote] waveshare pmic irq ready telemetry=%d power=%d\n",
                  g_pmicReady ? 1 : 0,
                  powerStatusIrqReady ? 1 : 0);
}

void serviceImuMotion(RemoteInputState* input) {
    if(input == nullptr || !g_imuReady) {
        return;
    }

    if(g_imuInterruptPending && !g_touchStablePressed) {
        g_imuInterruptPending = false;
        g_imuLastPollAt = millis();
        g_imu.update();
    }

    if(g_motionActivityPending && !g_touchStablePressed) {
        input->motionDetected = true;
        g_motionActivityPending = false;
    }
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
    if(input == nullptr || !g_pmicButtonReady) {
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

    uint8_t irqStatus2 = 0;
    if(!readI2cRegister(kPmicAddress, kPmicIntStatus2Reg, &irqStatus2)) {
        g_pmicIrqLatched = false;
        return;
    }
    uint8_t irqStatus3 = 0;
    const bool hasStatus3 = readI2cRegister(kPmicAddress, kPmicIntStatus3Reg, &irqStatus3);
    const bool shortPress = (irqStatus2 & kPmicPkeyShortMask) != 0;
    const bool longPress = (irqStatus2 & kPmicPkeyLongMask) != 0;
    input->powerStatusChanged =
        (irqStatus2 & kPmicPowerStatus2Mask) != 0 ||
        (hasStatus3 && (irqStatus3 & kPmicPowerStatus3Mask) != 0);
    clearPmicButtonIrqStatus();
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

bool pingTouchController(const WavesharePinProfile& profile) {
    const RemoteBoardPins& pins = profile.pins;
    pinMode(pins.touchReset, OUTPUT);
    pinMode(kBootButtonPin, INPUT_PULLUP);
    pinMode(pins.touchInt, INPUT_PULLUP);
    digitalWrite(pins.touchReset, LOW);
    delay(30);
    digitalWrite(pins.touchReset, HIGH);
    delay(kTouchResetSettleMs);
    if(!beginSharedI2cBus(pins.touchSda, pins.touchScl)) {
        Serial.printf("[remote] waveshare probe %s bus %d/%d init failed\n",
                      profile.key,
                      pins.touchSda,
                      pins.touchScl);
        return false;
    }
    for(uint8_t attempt = 1; attempt <= kTouchProbeAttempts; ++attempt) {
        Wire.beginTransmission(kTouchAddress);
        if(Wire.endTransmission() == 0) {
            if(attempt > 1) {
                Serial.printf("[remote] waveshare probe %s touch ready after %u attempts\n",
                              profile.key,
                              attempt);
            }
            return true;
        }
        delay(kTouchProbeRetryMs);
    }
    Serial.printf("[remote] waveshare probe %s touch controller not detected\n", profile.key);
    return false;
}

bool probeI2cDevice(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

String scanI2cDevices() {
    String devices;
    for(uint8_t address = 0x03; address < 0x78; ++address) {
        if(!probeI2cDevice(address)) {
            continue;
        }

        char formatted[8] = {};
        snprintf(formatted, sizeof(formatted), "0x%02X", address);
        if(!devices.isEmpty()) {
            devices += ' ';
        }
        devices += formatted;
    }

    if(devices.isEmpty()) {
        devices = "<none>";
    }
    return devices;
}

#if defined(REMOTE_WAVESHARE_I2C_DIAG)
void logDiagnosticI2cPairs() {
    constexpr int8_t kCandidatePins[] = {
        1,  2,  3,  8,  9,  10, 11, 13, 14, 15, 16, 17,
        18, 21, 38, 39, 40, 41, 42, 43, 44, 45, 46,
    };

    bool anyMatch = false;
    for(size_t sdaIndex = 0; sdaIndex < (sizeof(kCandidatePins) / sizeof(kCandidatePins[0])); ++sdaIndex) {
        for(size_t sclIndex = 0; sclIndex < (sizeof(kCandidatePins) / sizeof(kCandidatePins[0])); ++sclIndex) {
            const int8_t sda = kCandidatePins[sdaIndex];
            const int8_t scl = kCandidatePins[sclIndex];
            if(sda == scl) {
                continue;
            }

            Wire.begin(sda, scl, kI2cClock);
            delay(2);

            const bool touchReady = probeI2cDevice(kTouchAddress);
            const bool ioExpanderReady = probeI2cDevice(kIoExpanderAddress);
            const bool pmicReady = probeI2cDevice(AXP2101_SLAVE_ADDRESS);
            const bool rtcReady = probeI2cDevice(kRtcAddress);
            const bool imuReady = probeI2cDevice(kImuAddress);
            const bool codecReady = probeI2cDevice(kAudioCodecAddress);
            if(!(touchReady || ioExpanderReady || pmicReady || rtcReady || imuReady || codecReady)) {
                continue;
            }

            anyMatch = true;
            Serial.printf(
                "[remote] waveshare diag bus %d/%d touch=%d ioexp=%d pmic=%d rtc=%d imu=%d codec=%d\n",
                sda,
                scl,
                touchReady ? 1 : 0,
                ioExpanderReady ? 1 : 0,
                pmicReady ? 1 : 0,
                rtcReady ? 1 : 0,
                imuReady ? 1 : 0,
                codecReady ? 1 : 0);
        }
    }

    if(!anyMatch) {
        Serial.println("[remote] waveshare diag no candidate i2c pairs responded");
    }
}
#endif

bool probeWavesharePeripherals(const WavesharePinProfile& profile) {
    const RemoteBoardPins& pins = profile.pins;
    if(!beginSharedI2cBus(pins.touchSda, pins.touchScl)) {
        Serial.printf("[remote] waveshare profile %s bus %d/%d init failed\n",
                      profile.key,
                      pins.touchSda,
                      pins.touchScl);
        return false;
    }
    const String devices = scanI2cDevices();
    Serial.printf("[remote] waveshare profile %s bus %d/%d devices: %s\n",
                  profile.key,
                  pins.touchSda,
                  pins.touchScl,
                  devices.c_str());

    const bool ioExpanderReady = probeI2cDevice(kIoExpanderAddress);
    const bool pmicReady = probeI2cDevice(AXP2101_SLAVE_ADDRESS);
    const bool imuReady = probeI2cDevice(kImuAddress);
    if(ioExpanderReady || pmicReady || imuReady) {
        Serial.printf("[remote] waveshare profile %s fallback ioexp=%d pmic=%d imu=%d\n",
                      profile.key,
                      ioExpanderReady ? 1 : 0,
                      pmicReady ? 1 : 0,
                      imuReady ? 1 : 0);
        return true;
    }

    return false;
}

bool detectWaveshareProfile() {
    for(const WavesharePinProfile& profile : kPinProfiles) {
        if(pingTouchController(profile)) {
            selectProfile(profile, true);
            Serial.printf("[remote] waveshare selected profile %s via touch probe\n", profile.key);
            return true;
        }
        if(probeWavesharePeripherals(profile)) {
            selectProfile(profile, true);
            Serial.printf("[remote] waveshare selected profile %s via i2c fallback\n", profile.key);
            return true;
        }
    }

    selectProfile(kPinProfiles[0], false);
#if defined(REMOTE_WAVESHARE_I2C_DIAG)
    if(!g_loggedDiagnosticI2cPairs) {
        g_loggedDiagnosticI2cPairs = true;
        logDiagnosticI2cPairs();
    }
#endif
    return false;
}

}  // namespace

bool waveshare_probe() {
    return detectWaveshareProfile();
}

bool waveshare_init() {
    if(!g_profileDetected) {
        detectWaveshareProfile();
    }

    Serial.printf("[remote] waveshare init start profile=%s (%s)\n",
                  activeProfileKey(),
                  g_activeProfile != nullptr ? g_activeProfile->label : "unknown");
    if(!beginSharedI2cBus(g_boardPins.touchSda, g_boardPins.touchScl)) {
        Serial.println("[remote] waveshare i2c init failed");
        return false;
    }
    pinMode(kBootButtonPin, INPUT_PULLUP);

    pinMode(g_boardPins.touchReset, OUTPUT);
    pinMode(g_boardPins.touchInt, INPUT_PULLUP);
    digitalWrite(g_boardPins.touchReset, LOW);
    delay(30);
    digitalWrite(g_boardPins.touchReset, HIGH);
    delay(50);
    delay(kTouchResetSettleMs);

    g_touch.setPins(g_boardPins.touchReset, g_boardPins.touchInt);
    bool touchReady = false;
    for(uint8_t attempt = 1; attempt <= kTouchInitAttempts; ++attempt) {
        if(g_touch.begin(Wire, kTouchAddress, -1, -1)) {
            touchReady = true;
            if(attempt > 1) {
                Serial.printf("[remote] waveshare touch init succeeded on attempt %u\n", attempt);
            }
            break;
        }
        Serial.printf("[remote] waveshare touch init failed on attempt %u/%u\n",
                      attempt,
                      kTouchInitAttempts);
        digitalWrite(g_boardPins.touchReset, LOW);
        delay(20);
        digitalWrite(g_boardPins.touchReset, HIGH);
        delay(kTouchInitRetryMs);
    }
    if(!touchReady) {
        Serial.println("[remote] waveshare touch init gave up");
        return false;
    }
    g_touch.reset();
    delay(120);
    pinMode(g_boardPins.touchInt, INPUT_PULLUP);
    g_touch.setMaxCoordinates(kDisplayWidth, kDisplayHeight);
    g_touch.setMirrorXY(true, true);
    Serial.printf("[remote] waveshare touch ready int=%d\n",
                  digitalRead(g_boardPins.touchInt) == LOW ? 0 : 1);

    g_ioExpanderReady = initIoExpander();
    if(!g_ioExpanderReady) {
        Serial.println("[remote] waveshare power button sense unavailable");
    }

    g_imuReady = initImuMotion();
    setImuWakeSupport(g_imuReady);
    initPmicButtonIrq();

    if(!ensureDisplayObjects()) {
        return false;
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
    g_touchSuccessCount = 0;
    g_touchStablePressed = false;
    g_touchPressSampleCount = 0;
    g_touchReleaseSampleCount = 0;
    g_touchStableX = 0;
    g_touchStableY = 0;
    g_touchLastGoodSampleAt = 0;
    g_pmicLastSenseAt = 0;
    g_pmicLastBatteryLevelAt = 0;
    g_pmicBatteryCache = {};
    g_pmicIrqLatched = false;
    g_imuInterruptPending = false;
    g_imuLastPollAt = 0;
    g_motionActivityPending = false;
    attachInterrupt(kBootButtonPin, waveshareBootButtonIsr, CHANGE);
    attachInterrupt(g_boardPins.touchInt, waveshareTouchIsr, FALLING);
    if(g_imuReady) {
        attachInterrupt(g_boardPins.imuInt, waveshareImuIsr, CHANGE);
    }
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
    const bool touchIrqActive = digitalRead(g_boardPins.touchInt) == LOW;
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
    serviceImuMotion(input);
}

bool waveshare_input_pending() {
    return g_bootButtonInterruptPending ||
           (g_bootButton.initialized && g_bootButton.lastSamplePressed != g_bootButton.stablePressed) ||
           g_touchInterruptPending || g_touchStablePressed || g_imuInterruptPending ||
           g_motionActivityPending;
}

RemoteCanvas& waveshare_canvas() {
    return g_canvas;
}

const RemoteBoardInfo& waveshare_info() {
    return kBoardInfo;
}

const RemoteBoardServices& waveshare_services() {
    return g_boardServices;
}

const RemoteBoardPins& waveshare_pins() {
    return g_boardPins;
}

const RemoteBoardFeatureMap& waveshare_feature_map() {
    return g_boardFeatureMap;
}

void waveshare_apply_indicator(const RemoteIndicatorState&) {}

bool waveshare_set_backlight(uint8_t level) {
    if(g_display == nullptr) {
        return false;
    }
    g_display->setBrightness(level);
    return true;
}

RemoteBatteryStatus waveshare_battery_status() {
    RemoteBatteryStatus status = {};
    status.supported = g_pmicReady;
    if(!g_pmicReady) {
        return status;
    }

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    if(!readI2cRegister(kPmicAddress, kPmicStatus1Reg, &status1) ||
       !readI2cRegister(kPmicAddress, kPmicStatus2Reg, &status2)) {
        status.supported = false;
        return status;
    }

    const uint8_t chargeState = static_cast<uint8_t>(status2 >> 5);
    status.batteryPresent = (status1 & 0x08) != 0;
    status.usbPresent = (status1 & 0x20) != 0 && (status2 & 0x08) == 0;
    status.charging = chargeState == 0x01;
    status.discharging = chargeState == 0x02;
    status.standby = chargeState == 0x00;

    status.percent = status.batteryPresent ? g_pmicBatteryCache.percent : -1;
    status.millivolts = status.batteryPresent ? g_pmicBatteryCache.millivolts : -1;
    status.vbusMillivolts = status.usbPresent ? g_pmicBatteryCache.vbusMillivolts : -1;
    status.systemMillivolts = g_pmicBatteryCache.systemMillivolts;

    const uint32_t now = millis();
    const bool powerPathChanged =
        status.batteryPresent != g_pmicBatteryCache.batteryPresent ||
        status.usbPresent != g_pmicBatteryCache.usbPresent;
    const bool levelDue = g_pmicLastBatteryLevelAt == 0 ||
                          static_cast<uint32_t>(now - g_pmicLastBatteryLevelAt) >= remote_config::kBatteryPollMs ||
                          powerPathChanged;
    if(levelDue) {
        status.percent = -1;
        status.millivolts = -1;
        status.vbusMillivolts = -1;
        status.systemMillivolts = -1;

        uint16_t raw = 0;
        if(status.batteryPresent &&
           readPmicH5L8(kPmicBattVoltageHighReg, kPmicBattVoltageLowReg, &raw)) {
            status.millivolts = static_cast<int32_t>(raw);
        }
        if(status.usbPresent &&
           readPmicH6L8(kPmicVbusVoltageHighReg, kPmicVbusVoltageLowReg, &raw)) {
            status.vbusMillivolts = static_cast<int32_t>(raw);
        }
        if(readPmicH6L8(kPmicSystemVoltageHighReg, kPmicSystemVoltageLowReg, &raw)) {
            status.systemMillivolts = static_cast<int32_t>(raw);
        }

        uint8_t percent = 0;
        if(status.batteryPresent &&
           readI2cRegister(kPmicAddress, kPmicBatteryPercentReg, &percent) &&
           percent <= 100) {
            status.percent = static_cast<int8_t>(percent);
        }
        g_pmicLastBatteryLevelAt = now;
    }

    g_pmicBatteryCache = status;
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
