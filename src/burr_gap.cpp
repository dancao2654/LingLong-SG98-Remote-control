#include "burr_gap.h"

#include <driver/gpio.h>

#include "controller_board.h"
#include "display.h"
#include "pdata.h"

namespace {
constexpr uint8_t kFrameBits = 24;
constexpr uint32_t kFrameGapUs = 5000;
constexpr uint32_t kLiveTimeoutMs = 3000;
constexpr int32_t kStableFrameToleranceUm = 10;
constexpr uint8_t kStableFrameCountRequired = 3;
constexpr uint8_t kLargeJumpStableFrameCountRequired = 12;
constexpr uint8_t kHugeJumpStableFrameCountRequired = 20;
constexpr uint32_t kStableFrameWindowMs = 650;
constexpr int32_t kContinuousSmallJumpUm = 120;
constexpr int32_t kContinuousHugeJumpUm = 2000;
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
bool gManualBaseSet = false;
int32_t gDisplayUm = 0;
int32_t gRawUm = 0;
int32_t gBaseUm = 0;
int32_t gLastPersistedBaseUm = 0;
uint32_t gLastFrameMs = 0;
uint32_t gReadChangeSequence = 0;
uint32_t gLastReadChangeMs = 0;
uint32_t gTareAtMs = 0;
uint8_t gClockPin = 0;
uint8_t gDataPin = 0;
bool gFilterHavePending = false;
int32_t gFilterPendingUm = 0;
uint8_t gFilterPendingCount = 0;
uint32_t gFilterPendingMs = 0;

int32_t abs32(int32_t value) {
    return value < 0 ? -value : value;
}

void requestDisplayRefresh() {
    disp_updateRequired = true;
}

int32_t lcdUmFromDisplayUm(int32_t value) {
    return (value / 10) * 10;
}

void noteActiveReadDisplayChange(bool wasKnown, int32_t previousDisplayUm) {
    const int32_t previousLcdUm = lcdUmFromDisplayUm(previousDisplayUm);
    const int32_t currentLcdUm = lcdUmFromDisplayUm(gDisplayUm);
    if(wasKnown && previousLcdUm == currentLcdUm) {
        return;
    }

    ++gReadChangeSequence;
    if(gReadChangeSequence == 0) {
        ++gReadChangeSequence;
    }
    gLastReadChangeMs = millis();
}

void persistBase(bool force) {
    if(!gKnown) return;
    if(!force && abs32(gBaseUm - gLastPersistedBaseUm) < 10) {
        return;
    }

    pdata_write_burr_gap_seen(true);
    pdata_write_burr_gap_last_um(gBaseUm);
    pdata_write_burr_gap_offset_um(gBaseUm);
    pdata_write_burr_gap_manual_base(gManualBaseSet);
    gLastPersistedBaseUm = gBaseUm;
}

bool decodeFrameUm(uint32_t frame, int32_t* outUm) {
    if(outUm == nullptr) return false;

    const bool inchMode = (frame & (1UL << 23)) != 0;
    const bool negative = (frame & (1UL << 20)) != 0;
    const uint32_t counts = frame & 0x000FFFFFUL;
    int64_t um = 0;

    if(inchMode) {
        // This gauge reports 0.00025 in per count.
        um = (static_cast<int64_t>(counts) * 635 + 50) / 100;
    } else {
        // This gauge reports 0.005 mm per count.
        um = static_cast<int64_t>(counts) * 5;
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

bool selectGaugePins() {
    const ControllerBoardBurrGapInput& input = controller_board_burr_gap_input();
    if(!controller_board_pin_valid(input.clock) || !controller_board_pin_valid(input.data)) {
        return false;
    }

    gClockPin = static_cast<uint8_t>(input.clock);
    gDataPin = static_cast<uint8_t>(input.data);
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
        !gManualBaseSet &&
        continuous &&
        abs32(rawUm) <= kTareNearZeroUm &&
        abs32(previousRawUm) >= kTarePreviousThresholdUm &&
        abs32(rawUm - previousRawUm) >= kTareJumpThresholdUm;

    gRawUm = rawUm;
    gHaveRaw = true;
    gLive = true;
    gLastFrameMs = now;

    if(rawJumpedToZero) {
        // The indicator was zeroed while SG98 was running. Promote the last
        // displayed gap to the persistent base so future gauge readings stay
        // relative to that point.
        gBaseUm = wasKnown ? previousDisplayUm : 0;
        gDisplayUm = gBaseUm + rawUm;
        gKnown = true;
        gRecentlyTared = true;
        gTareAtMs = now;
        noteActiveReadDisplayChange(wasKnown, previousDisplayUm);
        persistBase(true);
        requestDisplayRefresh();
        return;
    }

    if(!gKnown) {
        gKnown = true;
    }

    gDisplayUm = gBaseUm + rawUm;
    noteActiveReadDisplayChange(wasKnown, previousDisplayUm);

    if(!wasKnown || !wasLive || abs32(gDisplayUm - previousDisplayUm) >= 10) {
        requestDisplayRefresh();
    }

    persistBase(!wasKnown);
}

void resetFrameFilter() {
    gFilterHavePending = false;
    gFilterPendingUm = 0;
    gFilterPendingCount = 0;
    gFilterPendingMs = 0;
}

uint8_t stableFrameCountRequiredFor(int32_t rawUm) {
    if(!gHaveRaw || !gLive) {
        return kStableFrameCountRequired;
    }

    const int32_t delta = abs32(rawUm - gRawUm);
    if(delta <= kContinuousSmallJumpUm) {
        return kStableFrameCountRequired;
    }
    if(delta >= kContinuousHugeJumpUm) {
        return kHugeJumpStableFrameCountRequired;
    }
    return kLargeJumpStableFrameCountRequired;
}

void acceptDecodedReading(int32_t rawUm) {
    const uint32_t now = millis();
    if(!gFilterHavePending ||
       (now - gFilterPendingMs) > kStableFrameWindowMs ||
       abs32(rawUm - gFilterPendingUm) > kStableFrameToleranceUm) {
        gFilterHavePending = true;
        gFilterPendingUm = rawUm;
        gFilterPendingCount = 1;
        gFilterPendingMs = now;
        return;
    }

    gFilterPendingUm = rawUm;
    gFilterPendingMs = now;
    const uint8_t requiredCount = stableFrameCountRequiredFor(rawUm);
    if(gFilterPendingCount < requiredCount) {
        gFilterPendingCount++;
    }
    if(gFilterPendingCount >= requiredCount) {
        applyRawReading(rawUm);
        resetFrameFilter();
    }
}

void IRAM_ATTR onGaugeClockFalling() {
    const uint32_t now = micros();

    portENTER_CRITICAL_ISR(&gFrameMux);
    if((now - gIsrLastEdgeUs) > kFrameGapUs) {
        gIsrBitIndex = 0;
        gIsrShift = 0;
    }

    if(gIsrBitIndex < kFrameBits) {
        if(gpio_get_level(static_cast<gpio_num_t>(gDataPin)) != 0) {
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
    gEnabled = selectGaugePins();
    if(!gEnabled) {
        return;
    }

    gKnown = pdata_read_burr_gap_seen();
    gBaseUm = pdata_read_burr_gap_last_um();
    gDisplayUm = gBaseUm;
    gLastPersistedBaseUm = gBaseUm;
    gManualBaseSet = pdata_has_burr_gap_manual_base()
                         ? pdata_read_burr_gap_manual_base()
                         : gKnown;
    resetFrameFilter();

    portENTER_CRITICAL(&gFrameMux);
    gIsrShift = 0;
    gIsrBitIndex = 0;
    gIsrLastEdgeUs = 0;
    gIsrFrame = 0;
    gIsrFrameReady = false;
    gIsrOverruns = 0;
    portEXIT_CRITICAL(&gFrameMux);

    pinMode(gDataPin, INPUT_PULLUP);
    pinMode(gClockPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(gClockPin), onGaugeClockFalling, FALLING);
}

void burr_gap_poll() {
    if(!gEnabled) return;

    uint32_t frame = 0;
    if(takeFrame(&frame)) {
        int32_t rawUm = 0;
        if(decodeFrameUm(frame, &rawUm)) {
            acceptDecodedReading(rawUm);
        }
    }

    const uint32_t now = millis();
    if(gLive && (now - gLastFrameMs) >= kLiveTimeoutMs) {
        gLive = false;
        resetFrameFilter();
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

bool burr_gap_manual_base_set() {
    return gEnabled && gManualBaseSet;
}

int32_t burr_gap_display_um() {
    return gDisplayUm;
}

int32_t burr_gap_display_lcd_um() {
    return lcdUmFromDisplayUm(gDisplayUm);
}

int32_t burr_gap_raw_um() {
    return gRawUm;
}

int32_t burr_gap_base_um() {
    return gBaseUm;
}

int32_t burr_gap_base_lcd_um() {
    return lcdUmFromDisplayUm(gBaseUm);
}

int32_t burr_gap_offset_um() {
    return gBaseUm;
}

uint32_t burr_gap_last_frame_ms() {
    return gLastFrameMs;
}

uint32_t burr_gap_read_change_sequence() {
    return gReadChangeSequence;
}

uint32_t burr_gap_last_read_change_ms() {
    return gLastReadChangeMs;
}

void burr_gap_set_base_um(int32_t gapUm) {
    if(!gEnabled) return;

    gManualBaseSet = true;
    gBaseUm = gapUm;
    gDisplayUm = (gHaveRaw && gLive) ? (gBaseUm + gRawUm) : gBaseUm;
    gKnown = true;
    gRecentlyTared = gapUm == 0;
    gTareAtMs = millis();
    persistBase(true);
    requestDisplayRefresh();
}

void burr_gap_set_current_gap_um(int32_t gapUm) {
    burr_gap_set_base_um(gapUm);
}
