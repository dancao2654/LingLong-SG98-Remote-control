#include "crowpanel_diag.h"

#include <Arduino.h>
#include <ESP32Encoder.h>
#include <LovyanGFX.hpp>
#include <Wire.h>

namespace {

constexpr uint8_t kDisplayWidth = 240;
constexpr uint8_t kDisplayHeight = 240;

constexpr int8_t kTouchSdaPin = 6;
constexpr int8_t kTouchSclPin = 7;
constexpr int8_t kTouchIntPin = 5;
constexpr int8_t kTouchResetPin = 13;
constexpr int8_t kDisplaySclkPin = 10;
constexpr int8_t kDisplayMosiPin = 11;
constexpr int8_t kDisplayDcPin = 3;
constexpr int8_t kDisplayCsPin = 9;
constexpr int8_t kDisplayResetPin = 14;
constexpr int8_t kBacklightPin = 46;
constexpr int8_t kPowerEnablePin = 40;
constexpr int8_t kAuxPowerPin1 = 1;
constexpr int8_t kAuxPowerPin2 = 2;
constexpr int8_t kEncoderAPin = 45;
constexpr int8_t kEncoderBPin = 42;
constexpr int8_t kEncoderButtonPin = 41;
constexpr int8_t kAuxI2cSdaPin = 38;
constexpr int8_t kAuxI2cSclPin = 39;

constexpr uint8_t kTouchAddress = 0x15;
constexpr uint8_t kOledAddressPrimary = 0x3C;
constexpr uint8_t kOledAddressSecondary = 0x3D;

constexpr uint32_t kTouchRetryMs = 5000;
constexpr uint32_t kLiveRefreshMs = 80;
constexpr uint32_t kDisplayStageMs = 2500;
constexpr uint8_t kLogLineCount = 10;

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
            cfg.pin_sclk = kDisplaySclkPin;
            cfg.pin_mosi = kDisplayMosiPin;
            cfg.pin_miso = -1;
            cfg.pin_dc = kDisplayDcPin;
            bus_.config(cfg);
            panel_.setBus(&bus_);
        }
        {
            auto cfg = panel_.config();
            cfg.pin_cs = kDisplayCsPin;
            cfg.pin_rst = kDisplayResetPin;
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

struct TouchSnapshot {
    bool ready = false;
    bool initWriteOk = false;
    bool reg01Ok = false;
    bool reg02Ok = false;
    bool blockOk = false;
    bool readA7Ok = false;
    bool readA8Ok = false;
    bool readA9Ok = false;
    uint8_t reg01 = 0;
    uint8_t reg02 = 0;
    uint8_t regA7 = 0;
    uint8_t regA8 = 0;
    uint8_t regA9 = 0;
    uint8_t block[4] = {};
    String scanSummary;
    String modeSummary;
};

CrowPanelDisplay g_display;
ESP32Encoder g_encoder;

String g_auxSummary = "Aux I2C: pending";
String g_touchSummary = "Touch: pending";
String g_touchLiveSummary = "Touch live: n/a";
String g_encoderSummary = "Encoder: pending";
String g_logLines[kLogLineCount];

TouchSnapshot g_touchSnapshot;
TwoWire* g_touchWire = nullptr;
int8_t g_touchWireSda = -1;
int8_t g_touchWireScl = -1;
bool g_displayReady = false;
bool g_touchConfigured = false;
bool g_buttonLast = true;
int32_t g_encoderLastRaw = 0;
uint32_t g_nextTouchProbeAt = 0;
uint32_t g_nextLiveRefreshAt = 0;
bool g_screenDirty = true;
uint8_t g_displayStage = 0;
uint32_t g_nextDisplayStageAt = 0;

enum class BacklightMode : uint8_t {
    Pwm255,
    DigitalHigh,
    DigitalLow,
};

uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return g_display.color565(r, g, b);
}

void appendLog(const String& line) {
    Serial.println(line);
    for(uint8_t i = 0; i + 1 < kLogLineCount; ++i) {
        g_logLines[i] = g_logLines[i + 1];
    }
    g_logLines[kLogLineCount - 1] = line;
    g_screenDirty = true;
}

String joinAddressList(TwoWire& wire, int8_t sda, int8_t scl, uint32_t frequency) {
    wire.begin(sda, scl, frequency);
    wire.setTimeOut(20);

    String found;
    for(uint8_t address = 1; address < 127; ++address) {
        wire.beginTransmission(address);
        if(wire.endTransmission() == 0) {
            if(!found.isEmpty()) {
                found += ",";
            }
            if(address < 16) {
                found += "0x0";
            } else {
                found += "0x";
            }
            found += String(address, HEX);
        }
    }
    found.toUpperCase();
    return found.isEmpty() ? String("none") : found;
}

bool addressSeen(const String& summary, uint8_t address) {
    String needle = "0x";
    if(address < 16) {
        needle += "0";
    }
    needle += String(address, HEX);
    needle.toUpperCase();
    return summary.indexOf(needle) >= 0;
}

void powerOnBoardPeripherals() {
    pinMode(kPowerEnablePin, OUTPUT);
    digitalWrite(kPowerEnablePin, LOW);

    pinMode(kAuxPowerPin1, OUTPUT);
    digitalWrite(kAuxPowerPin1, HIGH);

    pinMode(kAuxPowerPin2, OUTPUT);
    digitalWrite(kAuxPowerPin2, HIGH);

    delay(20);
}

void initBacklight() {
    ledcSetup(1, 5000, 8);
    ledcAttachPin(kBacklightPin, 1);
    ledcWrite(1, 255);
}

void setPanelPowerState(bool indicatorHigh, bool lcdRailHigh, bool out5vHigh) {
    pinMode(kPowerEnablePin, OUTPUT);
    digitalWrite(kPowerEnablePin, indicatorHigh ? HIGH : LOW);

    pinMode(kAuxPowerPin1, OUTPUT);
    digitalWrite(kAuxPowerPin1, lcdRailHigh ? HIGH : LOW);

    pinMode(kAuxPowerPin2, OUTPUT);
    digitalWrite(kAuxPowerPin2, out5vHigh ? HIGH : LOW);
}

void setBacklightMode(BacklightMode mode) {
    switch(mode) {
        case BacklightMode::Pwm255:
            initBacklight();
            break;
        case BacklightMode::DigitalHigh:
            ledcDetachPin(kBacklightPin);
            pinMode(kBacklightPin, OUTPUT);
            digitalWrite(kBacklightPin, HIGH);
            break;
        case BacklightMode::DigitalLow:
            ledcDetachPin(kBacklightPin);
            pinMode(kBacklightPin, OUTPUT);
            digitalWrite(kBacklightPin, LOW);
            break;
    }
}

void resetDisplayPanel() {
    pinMode(kDisplayResetPin, OUTPUT);
    digitalWrite(kDisplayResetPin, LOW);
    delay(20);
    digitalWrite(kDisplayResetPin, HIGH);
    delay(120);
}

void initDisplay() {
    g_display.init();
    g_display.initDMA();
    g_display.startWrite();
    g_display.setRotation(0);
    g_display.fillScreen(TFT_BLACK);
    g_display.setTextWrap(false);
    g_displayReady = true;
    g_screenDirty = true;
}

void setBacklightLevel(uint8_t level) {
    ledcWrite(1, level);
}

void showDisplayStage() {
    struct Stage {
        bool lcdRailHigh;
        bool out5vHigh;
        bool indicatorHigh;
        BacklightMode backlight;
        bool initPanel;
        uint16_t color;
        const char* label;
    };

    static const Stage kStages[] = {
        {true, true, false, BacklightMode::DigitalHigh, false, TFT_BLACK, "BL DIGITAL HIGH ONLY"},
        {true, true, false, BacklightMode::Pwm255, false, TFT_BLACK, "BL PWM255 ONLY"},
        {true, true, false, BacklightMode::DigitalHigh, true, TFT_RED, "INIT + BL DIGITAL HIGH + RED"},
        {true, true, false, BacklightMode::Pwm255, true, TFT_GREEN, "INIT + BL PWM255 + GREEN"},
        {true, false, false, BacklightMode::DigitalHigh, true, TFT_BLUE, "INIT + OUT5V LOW + BLUE"},
        {true, true, false, BacklightMode::DigitalLow, true, TFT_WHITE, "INIT + BL DIGITAL LOW + WHITE"},
    };

    const Stage& stage = kStages[g_displayStage % (sizeof(kStages) / sizeof(kStages[0]))];
    setPanelPowerState(stage.indicatorHigh, stage.lcdRailHigh, stage.out5vHigh);
    delay(80);
    setBacklightMode(stage.backlight);
    delay(80);

    if(stage.initPanel) {
        resetDisplayPanel();
        initDisplay();
    } else {
        g_displayReady = false;
    }

    if(g_displayReady) {
        g_display.fillScreen(stage.color);
        g_display.setTextSize(2);
        g_display.setTextColor(TFT_BLACK, stage.color);
        g_display.setTextDatum(middle_center);
        g_display.drawString(stage.label, kDisplayWidth / 2, kDisplayHeight / 2);
    }

    appendLog(String("Display stage: ") + stage.label);
}

void drawScreen() {
    if(!g_displayReady || !g_screenDirty) {
        return;
    }

    g_display.fillScreen(TFT_BLACK);
    g_display.setTextSize(1);
    g_display.setTextDatum(top_left);

    int16_t y = 4;
    auto drawLine = [&](const String& text, uint16_t color) {
        g_display.setTextColor(color, TFT_BLACK);
        g_display.drawString(text, 4, y);
        y += 12;
    };

    drawLine("CrowPanel 1.28 Diagnostic", color565(120, 220, 255));
    drawLine(g_auxSummary, color565(180, 180, 180));
    drawLine(g_touchSummary, color565(255, 230, 120));
    drawLine(g_touchLiveSummary, color565(120, 255, 120));
    drawLine(g_encoderSummary, color565(255, 180, 120));
    y += 4;
    drawLine("Recent log:", color565(180, 180, 255));

    for(uint8_t i = 0; i < kLogLineCount; ++i) {
        if(g_logLines[i].isEmpty()) {
            continue;
        }
        drawLine(g_logLines[i], color565(220, 220, 220));
    }

    g_screenDirty = false;
}

void resetTouchController() {
    pinMode(kTouchIntPin, OUTPUT);
    digitalWrite(kTouchIntPin, HIGH);
    delay(1);
    digitalWrite(kTouchIntPin, LOW);
    delay(1);

    pinMode(kTouchResetPin, OUTPUT);
    digitalWrite(kTouchResetPin, LOW);
    delay(10);
    digitalWrite(kTouchResetPin, HIGH);
    delay(300);
}

int writeTouchRegister(TwoWire& wire, uint8_t reg, uint8_t value) {
    wire.beginTransmission(kTouchAddress);
    wire.write(reg);
    wire.write(value);
    return wire.endTransmission();
}

bool readTouchRegisterRestart(TwoWire& wire, uint8_t reg, uint8_t* value) {
    if(value == nullptr) {
        return false;
    }

    wire.beginTransmission(kTouchAddress);
    wire.write(reg);
    if(wire.endTransmission(false) != 0) {
        return false;
    }

    const uint32_t started = millis();
    while(millis() - started < 30) {
        const size_t read = wire.requestFrom(static_cast<uint16_t>(kTouchAddress), static_cast<size_t>(1), true);
        if(read == 1 && wire.available()) {
            *value = static_cast<uint8_t>(wire.read());
            return true;
        }
        delay(1);
    }
    return false;
}

bool readTouchRegistersStop(TwoWire& wire, uint8_t reg, uint8_t* data, size_t length) {
    if(data == nullptr || length == 0) {
        return false;
    }

    wire.beginTransmission(kTouchAddress);
    wire.write(reg);
    if(wire.endTransmission(true) != 0) {
        return false;
    }

    const size_t read = wire.requestFrom(static_cast<uint16_t>(kTouchAddress), length, true);
    if(read != length) {
        while(wire.available()) {
            wire.read();
        }
        return false;
    }

    for(size_t i = 0; i < length; ++i) {
        data[i] = static_cast<uint8_t>(wire.read());
    }
    return true;
}

void configureTouchBus(TwoWire& wire, int8_t sda, int8_t scl, uint32_t frequency, bool setTimeout) {
    if(frequency == 0) {
        wire.begin(sda, scl);
    } else {
        wire.begin(sda, scl, frequency);
    }
    wire.setTimeOut(setTimeout ? 20 : 50);
}

TouchSnapshot probeTouchMode(TwoWire& wire,
                             int8_t sda,
                             int8_t scl,
                             const char* label,
                             uint32_t frequency,
                             bool setTimeout,
                             bool pullupAfterReset) {
    TouchSnapshot snapshot = {};
    snapshot.modeSummary = String(label);

    configureTouchBus(wire, sda, scl, frequency, setTimeout);
    resetTouchController();

    if(pullupAfterReset) {
        pinMode(kTouchIntPin, INPUT_PULLUP);
    }

    snapshot.scanSummary = joinAddressList(wire, sda, scl, frequency == 0 ? 100000U : frequency);
    if(!addressSeen(snapshot.scanSummary, kTouchAddress)) {
        return snapshot;
    }

    snapshot.initWriteOk = (writeTouchRegister(wire, 0xFE, 0xFF) == 0);
    snapshot.reg01Ok = readTouchRegisterRestart(wire, 0x01, &snapshot.reg01);
    snapshot.reg02Ok = readTouchRegisterRestart(wire, 0x02, &snapshot.reg02);
    snapshot.blockOk = readTouchRegistersStop(wire, 0x03, snapshot.block, sizeof(snapshot.block));
    snapshot.readA7Ok = readTouchRegisterRestart(wire, 0xA7, &snapshot.regA7);
    snapshot.readA8Ok = readTouchRegisterRestart(wire, 0xA8, &snapshot.regA8);
    snapshot.readA9Ok = readTouchRegisterRestart(wire, 0xA9, &snapshot.regA9);
    snapshot.ready = snapshot.initWriteOk && snapshot.reg02Ok;
    return snapshot;
}

String formatTouchSnapshot(const TouchSnapshot& snapshot) {
    String line = snapshot.modeSummary;
    line += " scan=";
    line += snapshot.scanSummary;
    line += " fe=";
    line += snapshot.initWriteOk ? "ok" : "fail";
    line += " r1=";
    line += snapshot.reg01Ok ? String(snapshot.reg01, HEX) : String("--");
    line += " r2=";
    line += snapshot.reg02Ok ? String(snapshot.reg02, HEX) : String("--");
    line.toUpperCase();
    return line;
}

void runAuxProbe() {
    const String found = joinAddressList(Wire, kAuxI2cSdaPin, kAuxI2cSclPin, 400000U);
    g_auxSummary = "Aux I2C: " + found;
    if(found.indexOf("0X3C") >= 0 || found.indexOf("0X3D") >= 0) {
        g_auxSummary += " OLED";
    }
    appendLog(g_auxSummary);
}

void runTouchProbe() {
    appendLog("Touch probe start");

    const String auxBusScan = joinAddressList(Wire, kAuxI2cSdaPin, kAuxI2cSclPin, 400000U);
    appendLog("Aux bus " + auxBusScan);

    TouchSnapshot auxBus = probeTouchMode(Wire, kAuxI2cSdaPin, kAuxI2cSclPin, "aux@38/39", 400000U, true, true);
    appendLog(formatTouchSnapshot(auxBus));

    g_touchWire = &Wire;
    g_touchWireSda = kAuxI2cSdaPin;
    g_touchWireScl = kAuxI2cSclPin;
    g_touchSnapshot = auxBus;

    g_touchConfigured = g_touchSnapshot.ready;
    g_nextTouchProbeAt = millis() + kTouchRetryMs;

    g_touchSummary = "Touch: " + formatTouchSnapshot(g_touchSnapshot);
    if(g_touchSnapshot.blockOk) {
        g_touchSummary += " xy=";
        for(uint8_t i = 0; i < 4; ++i) {
            if(i != 0) {
                g_touchSummary += ",";
            }
            String value = String(g_touchSnapshot.block[i], HEX);
            value.toUpperCase();
            g_touchSummary += value;
        }
    }
    g_screenDirty = true;
}

void initEncoder() {
    pinMode(kEncoderAPin, INPUT);
    pinMode(kEncoderBPin, INPUT);
    pinMode(kEncoderButtonPin, INPUT_PULLUP);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    g_encoder.attachHalfQuad(kEncoderAPin, kEncoderBPin);
    g_encoder.setFilter(100);
    g_encoder.setCount(0);
    g_encoderLastRaw = 0;
    g_buttonLast = digitalRead(kEncoderButtonPin);
    g_encoderSummary = "Encoder: ready";
    appendLog("Encoder ready");
}

void updateEncoderStatus() {
    const int32_t raw = g_encoder.getCount();
    const int32_t delta = g_encoderLastRaw - raw;
    g_encoderLastRaw = raw;

    const bool buttonNow = digitalRead(kEncoderButtonPin);
    if(delta != 0 || buttonNow != g_buttonLast) {
        g_buttonLast = buttonNow;
        g_encoderSummary = "Encoder raw=" + String(raw) + " d=" + String(delta) +
                           " btn=" + String(buttonNow ? "up" : "down");
        appendLog(g_encoderSummary);
    }
}

void updateLiveTouch() {
    if(g_touchWire == nullptr) {
        if(millis() >= g_nextTouchProbeAt) {
            runTouchProbe();
        }
        return;
    }

    configureTouchBus(*g_touchWire, g_touchWireSda, g_touchWireScl, g_touchWire == &Wire ? 400000U : 100000U, true);

    if(!g_touchConfigured) {
        if(millis() >= g_nextTouchProbeAt) {
            runTouchProbe();
        }
        return;
    }

    uint8_t points = 0;
    uint8_t gesture = 0;
    uint8_t data[4] = {};
    const bool pointsOk = readTouchRegisterRestart(*g_touchWire, 0x02, &points);
    const bool gestureOk = readTouchRegisterRestart(*g_touchWire, 0x01, &gesture);
    const bool blockOk = readTouchRegistersStop(*g_touchWire, 0x03, data, sizeof(data));

    if(pointsOk && gestureOk && blockOk) {
        const uint16_t x = static_cast<uint16_t>(((data[0] & 0x0F) << 8) | data[1]);
        const uint16_t y = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
        g_touchLiveSummary =
            "Touch live: pts=" + String(points) + " gst=0x" + String(gesture, HEX) + " x=" + String(x) +
            " y=" + String(y) + " int=" + String(digitalRead(kTouchIntPin));
        g_touchLiveSummary.toUpperCase();
    } else {
        g_touchLiveSummary =
            "Touch live: read fail int=" + String(digitalRead(kTouchIntPin)) + " rst=" + String(digitalRead(kTouchResetPin));
    }

    g_screenDirty = true;
}

}  // namespace

void crowpanel_diag_setup() {
    Serial.begin(115200);
    delay(200);

    powerOnBoardPeripherals();

    appendLog("CrowPanel 1.28 diag boot");
    appendLog("Power rails enabled");
    showDisplayStage();
    g_nextDisplayStageAt = millis() + kDisplayStageMs;

    runAuxProbe();
    initEncoder();
    runTouchProbe();
}

void crowpanel_diag_loop() {
    const uint32_t now = millis();

    if(now >= g_nextDisplayStageAt) {
        g_displayStage = static_cast<uint8_t>(g_displayStage + 1);
        showDisplayStage();
        g_nextDisplayStageAt = now + kDisplayStageMs;
    }

    updateEncoderStatus();

    if(now >= g_nextLiveRefreshAt) {
        g_nextLiveRefreshAt = now + kLiveRefreshMs;
        updateLiveTouch();
    }

    delay(5);
}
