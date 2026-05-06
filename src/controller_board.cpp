#include "controller_board.h"

#include <Esp.h>
#include <esp_flash.h>

namespace {

constexpr ControllerBoardFeatures kAdafruitFeatures = {
    true,
    true,
    true,
    true,
    true,
};

constexpr ControllerBoardPins kAdafruitPins = {
    18,
    17,
    15,
    16,
    4,
    5,
    42,
    41,
    40,
    10,
    11,
    12,
    13,
    14,
    21,
    GPIO_NUM_4,
    GPIO_NUM_42,
    GPIO_NUM_41,
    GPIO_NUM_40,
    GPIO_NUM_15,
    GPIO_NUM_16,
};

constexpr ControllerBoardBurrGapInput kAdafruitBurrGapInput = {
    9,
    8,
    "controller EX_I2C: CLK=GPIO9/SCL DATA=GPIO8/SDA",
};

constexpr ControllerBoardInfo kAdafruitBoard = {
    ControllerBoardId::AdafruitFeather,
    "controller_box",
    "Adafruit Feather Controller",
    kAdafruitFeatures,
    kAdafruitPins,
    kAdafruitBurrGapInput,
};

constexpr ControllerBoardFeatures kHeadlessSimFeatures = {
    false,
    false,
    false,
    false,
    false,
};

constexpr ControllerBoardInfo kAdafruitHeadlessSimBoard = {
    ControllerBoardId::AdafruitFeather,
    "controller_box_headless_sim",
    "Adafruit Feather Headless Simulation",
    kHeadlessSimFeatures,
    kAdafruitPins,
    kAdafruitBurrGapInput,
};

constexpr ControllerBoardFeatures kWaveshareFeatures = {
    false,
    false,
    false,
    false,
    false,
};

constexpr ControllerBoardPins kWavesharePins = {
    18,
    17,
    21,
    21,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    GPIO_NUM_NC,
    GPIO_NUM_NC,
    GPIO_NUM_NC,
    GPIO_NUM_NC,
    GPIO_NUM_21,
    GPIO_NUM_21,
};

constexpr ControllerBoardBurrGapInput kWaveshareBurrGapInput = {
    1,
    2,
    "waveshare EXIO: CLK=O1/GPIO1 DATA=O2/GPIO2",
};

constexpr ControllerBoardInfo kWaveshareBoard = {
    ControllerBoardId::WaveshareRs485Can,
    "waveshare_rs485_can",
    "Waveshare RS485/CAN Controller",
    kWaveshareFeatures,
    kWavesharePins,
    kWaveshareBurrGapInput,
};

constexpr uint32_t kControllerBoxFlashBytes = 8U * 1024U * 1024U;

const ControllerBoardInfo* g_activeBoard = nullptr;
uint32_t g_detectedFlashSizeBytes = 0;
bool g_loggedBoardSelection = false;

uint32_t detectFlashSizeBytes() {
    uint32_t size = 0;
    // Use the chip's physical capacity, not the image-header flash size.
    if(esp_flash_get_physical_size(nullptr, &size) == ESP_OK && size != 0) {
        return size;
    }
    size = static_cast<uint32_t>(ESP.getFlashChipSize());
    if(size != 0) {
        return size;
    }
    if(esp_flash_get_size(nullptr, &size) == ESP_OK && size != 0) {
        return size;
    }
    return 0;
}

const ControllerBoardInfo* detectBoard() {
#if defined(SG98_CONTROLLER_SIMULATE_HEADLESS)
    return &kAdafruitHeadlessSimBoard;
#elif defined(SG98_CONTROLLER_FORCE_ADAFRUIT_FEATHER)
    return &kAdafruitBoard;
#elif defined(SG98_CONTROLLER_FORCE_WAVESHARE_RS485_CAN)
    return &kWaveshareBoard;
#else
    g_detectedFlashSizeBytes = detectFlashSizeBytes();
    if(g_detectedFlashSizeBytes > kControllerBoxFlashBytes) {
        return &kWaveshareBoard;
    }
    return &kAdafruitBoard;
#endif
}

void ensureBoardSelected() {
    if(g_activeBoard != nullptr) {
        return;
    }

    if(g_detectedFlashSizeBytes == 0) {
        g_detectedFlashSizeBytes = detectFlashSizeBytes();
    }
    g_activeBoard = detectBoard();
}

void logBoardSelectionIfNeeded() {
    if(g_loggedBoardSelection || g_activeBoard == nullptr) {
        return;
    }

    Serial.printf("[board] controller=%s flash=%lu bytes\n",
                  g_activeBoard->targetName,
                  static_cast<unsigned long>(g_detectedFlashSizeBytes));
    g_loggedBoardSelection = true;
}

}  // namespace

void controller_board_init() {
    ensureBoardSelected();
    logBoardSelectionIfNeeded();
}

bool controller_board_ready() {
    ensureBoardSelected();
    return g_activeBoard != nullptr;
}

const ControllerBoardInfo& controller_board_info() {
    ensureBoardSelected();
    return *g_activeBoard;
}

const ControllerBoardFeatures& controller_board_features() {
    return controller_board_info().features;
}

const ControllerBoardPins& controller_board_pins() {
    return controller_board_info().pins;
}

const ControllerBoardBurrGapInput& controller_board_burr_gap_input() {
    return controller_board_info().burrGap;
}

const char* controller_board_target_name() {
    return controller_board_info().targetName;
}

const char* controller_board_display_name() {
    return controller_board_info().displayName;
}

uint32_t controller_board_detected_flash_size_bytes() {
    ensureBoardSelected();
    return g_detectedFlashSizeBytes;
}
