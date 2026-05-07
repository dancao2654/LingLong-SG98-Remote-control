#pragma once

#include <Arduino.h>

#include "remote_canvas.h"

class TwoWire;
class SPIClass;
class HardwareSerial;

enum class RemoteBoardId : uint8_t {
    CrowPanelRotary,
    CrowPanelRotary21,
    WaveshareAmoled175,
    LilyGoT4S3H678,
};

enum class RemoteBoardCapability : uint32_t {
    Touch = 1u << 0,
    Encoder = 1u << 1,
    Indicator = 1u << 2,
    Backlight = 1u << 3,
    I2c = 1u << 4,
    Spi = 1u << 5,
    Uart = 1u << 6,
    BatterySense = 1u << 7,
    AudioInput = 1u << 8,
    AudioOutput = 1u << 9,
    MotionSensor = 1u << 10,
    Rtc = 1u << 11,
    Pmic = 1u << 12,
    DeepSleep = 1u << 13,
    WakeSource = 1u << 14,
    Haptics = 1u << 15,
    ExpansionGpio = 1u << 16,
    ExpansionI2c = 1u << 17,
    ExpansionSpi = 1u << 18,
    ExpansionUart = 1u << 19,
    UsbDetect = 1u << 20,
    ChargeState = 1u << 21,
    UsbConsole = 1u << 22,
};

enum class RemoteBoardSupportLevel : uint8_t {
    None = 0,
    Present = 1,
    Mapped = 2,
    Implemented = 3,
};

enum class RemoteWakeSource : uint32_t {
    Touch = 1u << 0,
    PrimaryButton = 1u << 1,
    EncoderSwitch = 1u << 2,
    Timer = 1u << 3,
    Motion = 1u << 4,
    Usb = 1u << 5,
};

struct RemoteTouchState {
    bool pressed = false;
    bool justPressed = false;
    bool justReleased = false;
    int16_t x = 0;
    int16_t y = 0;
};

struct RemoteInputState {
    int dialDelta = 0;
    bool motionDetected = false;
    bool displayRotationChanged = false;
    uint8_t displayRotation = 0;
    bool powerStatusChanged = false;
    bool primaryPressed = false;
    bool primaryShortPress = false;
    bool primaryLongPress = false;
    bool secondaryPressed = false;
    bool secondaryShortPress = false;
    bool secondaryLongPress = false;
    RemoteTouchState touch;
};

struct RemoteIndicatorState {
    bool linkOnline = false;
    bool running = false;
    bool gbwMode = false;
    bool hasError = false;
};

struct RemoteBatteryStatus {
    bool supported = false;
    bool batteryPresent = false;
    bool usbPresent = false;
    bool charging = false;
    bool discharging = false;
    bool standby = false;
    int8_t percent = -1;
    int32_t millivolts = -1;
    int32_t vbusMillivolts = -1;
    int32_t systemMillivolts = -1;
};

struct RemoteBoardInfo {
    RemoteBoardId id = RemoteBoardId::CrowPanelRotary;
    const char* name = "";
    uint16_t width = 0;
    uint16_t height = 0;
    bool hasTouch = false;
    bool hasEncoder = false;
    uint32_t capabilities = 0;

    RemoteBoardInfo() = default;
    RemoteBoardInfo(RemoteBoardId boardId,
                    const char* boardName,
                    uint16_t boardWidth,
                    uint16_t boardHeight,
                    bool boardHasTouch,
                    bool boardHasEncoder,
                    uint32_t boardCapabilities)
        : id(boardId),
          name(boardName),
          width(boardWidth),
          height(boardHeight),
          hasTouch(boardHasTouch),
          hasEncoder(boardHasEncoder),
          capabilities(boardCapabilities) {}
};

struct RemoteBoardServices {
    TwoWire* i2c = nullptr;
    SPIClass* spi = nullptr;
    HardwareSerial* uart = nullptr;
    Print* console = nullptr;
    int8_t i2cSda = -1;
    int8_t i2cScl = -1;
    int8_t spiSclk = -1;
    int8_t spiMosi = -1;
    int8_t spiMiso = -1;
    int8_t spiCs = -1;
    int8_t uartTx = -1;
    int8_t uartRx = -1;
    TwoWire* expansionI2c = nullptr;
    SPIClass* expansionSpi = nullptr;
    HardwareSerial* expansionUart = nullptr;
    int8_t expansionI2cSda = -1;
    int8_t expansionI2cScl = -1;
    int8_t expansionSpiSclk = -1;
    int8_t expansionSpiMosi = -1;
    int8_t expansionSpiMiso = -1;
    int8_t expansionSpiCs = -1;
    int8_t expansionUartTx = -1;
    int8_t expansionUartRx = -1;
    bool consoleIsUsb = false;

    RemoteBoardServices() = default;
    RemoteBoardServices(TwoWire* boardI2c,
                        SPIClass* boardSpi,
                        HardwareSerial* boardUart,
                        Print* boardConsole,
                        int8_t boardI2cSda,
                        int8_t boardI2cScl,
                        int8_t boardSpiSclk,
                        int8_t boardSpiMosi,
                        int8_t boardSpiMiso,
                        int8_t boardSpiCs,
                        int8_t boardUartTx,
                        int8_t boardUartRx,
                        TwoWire* boardExpansionI2c,
                        SPIClass* boardExpansionSpi,
                        HardwareSerial* boardExpansionUart,
                        int8_t boardExpansionI2cSda,
                        int8_t boardExpansionI2cScl,
                        int8_t boardExpansionSpiSclk,
                        int8_t boardExpansionSpiMosi,
                        int8_t boardExpansionSpiMiso,
                        int8_t boardExpansionSpiCs,
                        int8_t boardExpansionUartTx,
                        int8_t boardExpansionUartRx,
                        bool boardConsoleIsUsb)
        : i2c(boardI2c),
          spi(boardSpi),
          uart(boardUart),
          console(boardConsole),
          i2cSda(boardI2cSda),
          i2cScl(boardI2cScl),
          spiSclk(boardSpiSclk),
          spiMosi(boardSpiMosi),
          spiMiso(boardSpiMiso),
          spiCs(boardSpiCs),
          uartTx(boardUartTx),
          uartRx(boardUartRx),
          expansionI2c(boardExpansionI2c),
          expansionSpi(boardExpansionSpi),
          expansionUart(boardExpansionUart),
          expansionI2cSda(boardExpansionI2cSda),
          expansionI2cScl(boardExpansionI2cScl),
          expansionSpiSclk(boardExpansionSpiSclk),
          expansionSpiMosi(boardExpansionSpiMosi),
          expansionSpiMiso(boardExpansionSpiMiso),
          expansionSpiCs(boardExpansionSpiCs),
          expansionUartTx(boardExpansionUartTx),
          expansionUartRx(boardExpansionUartRx),
          consoleIsUsb(boardConsoleIsUsb) {}
};

struct RemoteBoardPins {
    int8_t touchSda = -1;
    int8_t touchScl = -1;
    int8_t touchInt = -1;
    int8_t touchReset = -1;
    int8_t displaySclk = -1;
    int8_t displayMosi = -1;
    int8_t displayMiso = -1;
    int8_t displayDc = -1;
    int8_t displayCs = -1;
    int8_t displayReset = -1;
    int8_t displayData0 = -1;
    int8_t displayData1 = -1;
    int8_t displayData2 = -1;
    int8_t displayData3 = -1;
    int8_t displayTe = -1;
    int8_t backlight = -1;
    int8_t powerEnable = -1;
    int8_t encoderA = -1;
    int8_t encoderB = -1;
    int8_t encoderSwitch = -1;
    int8_t indicatorData = -1;
    int8_t batterySense = -1;
    int8_t primaryButton = -1;
    int8_t secondaryButton = -1;
    int8_t usbDetect = -1;
    int8_t chargeState = -1;
    int8_t pmicInt = -1;
    int8_t imuInt = -1;
    int8_t rtcInt = -1;
    int8_t audioBclk = -1;
    int8_t audioLrclk = -1;
    int8_t audioDin = -1;
    int8_t audioDout = -1;
    int8_t audioMclk = -1;
    int8_t audioEnable = -1;
    int8_t hapticEnable = -1;
    int8_t hapticPwm = -1;
    int8_t expansionI2cSda = -1;
    int8_t expansionI2cScl = -1;
    int8_t expansionSpiSclk = -1;
    int8_t expansionSpiMosi = -1;
    int8_t expansionSpiMiso = -1;
    int8_t expansionSpiCs = -1;
    int8_t expansionUartTx = -1;
    int8_t expansionUartRx = -1;
    int8_t expansionGpio0 = -1;
    int8_t expansionGpio1 = -1;
    int8_t expansionGpio2 = -1;
    int8_t expansionGpio3 = -1;
};

struct RemoteBoardPowerMap {
    RemoteBoardSupportLevel mainPower = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel backlight = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel pmic = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel deepSleep = RemoteBoardSupportLevel::None;
    uint32_t wakeSources = 0;

    RemoteBoardPowerMap() = default;
    RemoteBoardPowerMap(RemoteBoardSupportLevel boardMainPower,
                        RemoteBoardSupportLevel boardBacklight,
                        RemoteBoardSupportLevel boardPmic,
                        RemoteBoardSupportLevel boardDeepSleep,
                        uint32_t boardWakeSources)
        : mainPower(boardMainPower),
          backlight(boardBacklight),
          pmic(boardPmic),
          deepSleep(boardDeepSleep),
          wakeSources(boardWakeSources) {}
};

struct RemoteBoardBatteryMap {
    RemoteBoardSupportLevel voltage = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel usbPresent = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel chargeState = RemoteBoardSupportLevel::None;

    RemoteBoardBatteryMap() = default;
    RemoteBoardBatteryMap(RemoteBoardSupportLevel boardVoltage,
                          RemoteBoardSupportLevel boardUsbPresent,
                          RemoteBoardSupportLevel boardChargeState)
        : voltage(boardVoltage),
          usbPresent(boardUsbPresent),
          chargeState(boardChargeState) {}
};

struct RemoteBoardAudioMap {
    RemoteBoardSupportLevel input = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel output = RemoteBoardSupportLevel::None;
    bool digitalMic = false;
    bool codecOnI2c = false;

    RemoteBoardAudioMap() = default;
    RemoteBoardAudioMap(RemoteBoardSupportLevel boardInput,
                        RemoteBoardSupportLevel boardOutput,
                        bool boardDigitalMic,
                        bool boardCodecOnI2c)
        : input(boardInput),
          output(boardOutput),
          digitalMic(boardDigitalMic),
          codecOnI2c(boardCodecOnI2c) {}
};

struct RemoteBoardMotionMap {
    RemoteBoardSupportLevel imu = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel rtc = RemoteBoardSupportLevel::None;

    RemoteBoardMotionMap() = default;
    RemoteBoardMotionMap(RemoteBoardSupportLevel boardImu, RemoteBoardSupportLevel boardRtc)
        : imu(boardImu), rtc(boardRtc) {}
};

struct RemoteBoardHapticsMap {
    RemoteBoardSupportLevel vibrator = RemoteBoardSupportLevel::None;

    RemoteBoardHapticsMap() = default;
    explicit RemoteBoardHapticsMap(RemoteBoardSupportLevel boardVibrator) : vibrator(boardVibrator) {}
};

struct RemoteBoardExpansionMap {
    RemoteBoardSupportLevel i2c = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel spi = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel uart = RemoteBoardSupportLevel::None;
    uint8_t gpioCount = 0;

    RemoteBoardExpansionMap() = default;
    RemoteBoardExpansionMap(RemoteBoardSupportLevel boardI2c,
                            RemoteBoardSupportLevel boardSpi,
                            RemoteBoardSupportLevel boardUart,
                            uint8_t boardGpioCount)
        : i2c(boardI2c), spi(boardSpi), uart(boardUart), gpioCount(boardGpioCount) {}
};

struct RemoteBoardDebugMap {
    RemoteBoardSupportLevel console = RemoteBoardSupportLevel::None;
    bool usbConsole = false;

    RemoteBoardDebugMap() = default;
    RemoteBoardDebugMap(RemoteBoardSupportLevel boardConsole, bool boardUsbConsole)
        : console(boardConsole), usbConsole(boardUsbConsole) {}
};

struct RemoteBoardInputMap {
    RemoteBoardSupportLevel touch = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel encoder = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel primaryButton = RemoteBoardSupportLevel::None;
    RemoteBoardSupportLevel secondaryButton = RemoteBoardSupportLevel::None;
    bool touchDial = false;
    bool touchScroll = false;

    RemoteBoardInputMap() = default;
    RemoteBoardInputMap(RemoteBoardSupportLevel boardTouch,
                        RemoteBoardSupportLevel boardEncoder,
                        RemoteBoardSupportLevel boardPrimaryButton,
                        RemoteBoardSupportLevel boardSecondaryButton,
                        bool boardTouchDial,
                        bool boardTouchScroll)
        : touch(boardTouch),
          encoder(boardEncoder),
          primaryButton(boardPrimaryButton),
          secondaryButton(boardSecondaryButton),
          touchDial(boardTouchDial),
          touchScroll(boardTouchScroll) {}
};

struct RemoteBoardFeatureMap {
    RemoteBoardPowerMap power;
    RemoteBoardBatteryMap battery;
    RemoteBoardAudioMap audio;
    RemoteBoardMotionMap motion;
    RemoteBoardHapticsMap haptics;
    RemoteBoardExpansionMap expansion;
    RemoteBoardDebugMap debug;
    RemoteBoardInputMap input;

    RemoteBoardFeatureMap() = default;
    RemoteBoardFeatureMap(const RemoteBoardPowerMap& boardPower,
                          const RemoteBoardBatteryMap& boardBattery,
                          const RemoteBoardAudioMap& boardAudio,
                          const RemoteBoardMotionMap& boardMotion,
                          const RemoteBoardHapticsMap& boardHaptics,
                          const RemoteBoardExpansionMap& boardExpansion,
                          const RemoteBoardDebugMap& boardDebug,
                          const RemoteBoardInputMap& boardInput)
        : power(boardPower),
          battery(boardBattery),
          audio(boardAudio),
          motion(boardMotion),
          haptics(boardHaptics),
          expansion(boardExpansion),
          debug(boardDebug),
          input(boardInput) {}
};

bool remote_board_init();
void remote_board_poll_input(RemoteInputState* input);
RemoteCanvas& remote_board_canvas();
const RemoteBoardInfo& remote_board_info();
const RemoteBoardServices& remote_board_services();
const RemoteBoardPins& remote_board_pins();
const RemoteBoardFeatureMap& remote_board_feature_map();
Print& remote_board_console();
void remote_board_apply_indicator(const RemoteIndicatorState& indicator);
bool remote_board_set_backlight(uint8_t level);
RemoteBatteryStatus remote_board_battery_status();
int32_t remote_board_battery_millivolts();
bool remote_board_input_pending();

inline bool remote_board_has_capability(RemoteBoardCapability capability) {
    return (remote_board_info().capabilities & static_cast<uint32_t>(capability)) != 0;
}

inline bool remote_board_supports(RemoteBoardSupportLevel level, RemoteBoardSupportLevel minimum) {
    return static_cast<uint8_t>(level) >= static_cast<uint8_t>(minimum);
}

inline bool remote_board_support_present(RemoteBoardSupportLevel level) {
    return remote_board_supports(level, RemoteBoardSupportLevel::Present);
}

inline bool remote_board_support_mapped(RemoteBoardSupportLevel level) {
    return remote_board_supports(level, RemoteBoardSupportLevel::Mapped);
}

inline bool remote_board_support_implemented(RemoteBoardSupportLevel level) {
    return remote_board_supports(level, RemoteBoardSupportLevel::Implemented);
}

inline bool remote_board_wake_supported(RemoteWakeSource source) {
    return (remote_board_feature_map().power.wakeSources & static_cast<uint32_t>(source)) != 0;
}
