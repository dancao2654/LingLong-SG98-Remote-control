#include "burr_gap.h"

#include <driver/gpio.h>

#include "controller_board.h"
#include "definitions.h"
#include "display.h"
#include "pdata.h"

namespace {
constexpr uint8_t kClockPin = BURR_GAP_CLK_PIN;
constexpr uint8_t kDataPin = BURR_GAP_DATA_PIN;
constexpr uint8_t kFrameBits = 24;
constexpr uint32_t kFrameGapUs = 5000;
constexpr uint32_t kLiveTimeoutMs = 1500;
constexpr uint32_t kPersistIntervalMs = 5000;
constexpr int32_t kPersistDeltaUm = 10;
constexpr int32_t kTareNearZeroUm = 30;
constexpr int32_t kTarePreviousThresholdUm = 100;
constexpr int32_t kTareJumpThresholdUm = 200;
constexpr int32_t kMaxReasonableGapUm = 60000;

portMUX_TYPE gFrameMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t gIsrShift = 0;
volatile uint8_t gIsrBitIndex = 0;
volatile uint32_t gIsrLastEdgeUs = 0;
volatile uint32_t gIsrFrame = 0;
volatile bool gIsrFrameReady = false;
volatile uint32_t gIsrOverruns = 0;

bool gEnabled = false;
bool gKnown = false;
bool gLive = false;
bool gHaveRaw = false;
bool gRecentlyTared = false;
int32_t gDisplayUm = 0;
int32_t gRawUm = 0;
int32_t gOffsetUm = 0;
int32_t gLastPersistedGapUm = 0;
uint32_t gLastFrameMs = 0;
uint32_t gLastPersistMs = 0;
uint32_t gTareAtMs = 0;

int32_t abs32(int32_t value) {
    return value < 0 ? -value : value;
}

void requestDisplayRefresh() {
    disp_updateRequired = true;
}

void persistState(bool force) {
    if(!gKnown) return;

    const uint32_t now = millis();
    if(!force &&
       (now - gLastPersistMs) < kPersistIntervalMs &&
       abs32(gDisplayUm - gLastPersistedGapUm) < kPersistDeltaUm) {
        return;
    }

    pdata_write_burr_gap_seen(true);
    pdata_write_burr_gap_last_um(gDisplayUm);
    pdata_write_burr_gap_offset_um(gOffsetUm);
    gLastPersistedGapUm = gDisplayUm;
    gLastPersistMs = now;
}

bool decodeFrameUm(uint32_t frame, int32_t* outUm) {
    if(outUm == nullptr) return false;

    const bool inchMode = (frame & (1UL << 23)) != 0;
    const bool negative = (frame & (1UL << 20)) != 0;
    const uint32_t counts = frame & 0x000FFFFFUL;
    int64_t um = 0;

    if(inchMode) {
        // Common caliper/indicator stream: 1 count = 0.0005 in = 12.7 um.
        um = (static_cast<int64_t>(counts) * 127 + 5) / 10;
    } else {
        // Common metric stream: 1 count = 0.01 mm = 10 um.
        um = static_cast<int64_t>(counts) * 10;
    }

    if(negative) {
        um = -um;
    }

    if(um > kMaxReasonableGapUm || um < -kMaxReasonableGapUm) {
        return false;
    }

    *outUm = static_cast<int32_t>(um);
    return true;
}

bool takeFrame(uint32_t* frame) {
    if(frame == nullptr) return false;

    bool ready = false;
    portENTER_CRITICAL(&gFrameMux);
    if(gIsrFrameReady) {
        *frame = gIsrFrame;
        gIsrFrameReady = false;
        ready = true;
    }
    portEXIT_CRITICAL(&gFrameMux);
    return ready;
}

void applyRawReading(int32_t rawUm) {
    const uint32_t now = millis();
    const bool wasKnown = gKnown;
    const bool wasLive = gLive;
    const int32_t previousDisplayUm = gDisplayUm;
    const int32_t previousRawUm = gRawUm;
    const bool continuous = gHaveRaw && gLive && (now - gLastFrameMs) <= kLiveTimeoutMs;
    const bool rawJumpedToZero =
        continuous &&
        abs32(rawUm) <= kTareNearZeroUm &&
        abs32(previousRawUm) >= kTarePreviousThresholdUm &&
        abs32(rawUm - previousRawUm) >= kTareJumpThresholdUm;

    gRawUm = rawUm;
    gHaveRaw = true;
    gLive = true;
    gLastFrameMs = now;

    if(rawJumpedToZero) {
        gOffsetUm = 0;
        gDisplayUm = 0;
        gKnown = true;
        gRecentlyTared = true;
        gTareAtMs = now;
        persistState(true);
        requestDisplayRefresh();
        return;
    }

    if(!continuous && gKnown) {
        gOffsetUm = gDisplayUm - rawUm;
    } else if(!gKnown) {
        gKnown = true;
    }

    gDisplayUm = rawUm + gOffsetUm;

    if(!wasKnown || !wasLive || abs32(gDisplayUm - previousDisplayUm) >= kPersistDeltaUm) {
        requestDisplayRefresh();
    }

    persistState(!wasKnown);
}

void IRAM_ATTR onGaugeClockFalling() {
    const uint32_t now = micros();

    portENTER_CRITICAL_ISR(&gFrameMux);
    if((now - gIsrLastEdgeUs) > kFrameGapUs) {
        gIsrBitIndex = 0;
        gIsrShift = 0;
    }

    if(gIsrBitIndex < kFrameBits) {
        if(gpio_get_level(static_cast<gpio_num_t>(kDataPin)) != 0) {
            gIsrShift |= (1UL << gIsrBitIndex);
        }
        gIsrBitIndex++;
    }

    if(gIsrBitIndex >= kFrameBits) {
        if(gIsrFrameReady) {
            gIsrOverruns++;
        }
        gIsrFrame = gIsrShift;
        gIsrFrameReady = true;
        gIsrBitIndex = 0;
        gIsrShift = 0;
    }

    gIsrLastEdgeUs = now;
    portEXIT_CRITICAL_ISR(&gFrameMux);
}

}  // namespace

void burr_gap_init() {
    gEnabled = controller_board_info().id == ControllerBoardId::AdafruitFeather &&
               controller_board_has_display();
    if(!gEnabled) {
        return;
    }

    gKnown = pdata_read_burr_gap_seen();
    gDisplayUm = pdata_read_burr_gap_last_um();
    gOffsetUm = pdata_read_burr_gap_offset_um();
    gLastPersistedGapUm = gDisplayUm;
    gLastPersistMs = millis();

    pinMode(kDataPin, INPUT);
    pinMode(kClockPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(kClockPin), onGaugeClockFalling, FALLING);
}

void burr_gap_poll() {
    if(!gEnabled) return;

    uint32_t frame = 0;
    if(takeFrame(&frame)) {
        int32_t rawUm = 0;
        if(decodeFrameUm(frame, &rawUm)) {
            applyRawReading(rawUm);
        }
    }

    const uint32_t now = millis();
    if(gLive && (now - gLastFrameMs) > kLiveTimeoutMs) {
        gLive = false;
        persistState(true);
        requestDisplayRefresh();
    }

    if(gRecentlyTared && (now - gTareAtMs) > 1800) {
        gRecentlyTared = false;
        requestDisplayRefresh();
    }
}

bool burr_gap_enabled() {
    return gEnabled;
}

bool burr_gap_has_value() {
    return gEnabled && gKnown;
}

bool burr_gap_live() {
    return gEnabled && gLive;
}

bool burr_gap_recently_tared() {
    return gEnabled && gRecentlyTared;
}

int32_t burr_gap_display_um() {
    return gDisplayUm;
}

int32_t burr_gap_raw_um() {
    return gRawUm;
}

int32_t burr_gap_offset_um() {
    return gOffsetUm;
}

uint32_t burr_gap_last_frame_ms() {
    return gLastFrameMs;
}

void burr_gap_set_current_gap_um(int32_t gapUm) {
    if(!gEnabled) return;

    if(gHaveRaw && gLive) {
        gOffsetUm = gapUm - gRawUm;
    } else {
        gOffsetUm = 0;
    }
    gDisplayUm = gapUm;
    gKnown = true;
    gRecentlyTared = gapUm == 0;
    gTareAtMs = millis();
    persistState(true);
    requestDisplayRefresh();
}
