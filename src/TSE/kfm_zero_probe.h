#pragma once
#include <cstdint>

// Operator-present, zero-preset enable/reset experiment. This is deliberately
// separate from normal motion control and never issues a nonzero speed value.
namespace kfm_zero_probe {
constexpr uint32_t kDeadlineMs = 120000;
constexpr uint32_t kPollMs = 250;
constexpr uint32_t kGapMs = 500;
constexpr uint32_t kReturnObservationMs = 2000;
enum class Phase : uint8_t { Idle, Preflight, VerifyHold, WaitingForGap, AwaitReturn, VerifyReturn, Complete, Cancelled, Failed };
struct Sample {
    uint16_t source[4]; // PA22..25, signed speed words remain raw here
    uint16_t forced[5];
    uint16_t inputs[4];
    uint16_t virtualInputs[2];
    uint16_t mode;
    uint16_t softwareEnable;
    int16_t rpm;
    uint16_t alarm;
    uint16_t busVoltageRaw;
    uint16_t operatingStatusRaw;
    uint16_t inputsStatusRaw;
};
struct View {
    Phase phase = Phase::Idle;
    const char* reason = "not_started";
    uint32_t startedAt = 0;
    uint32_t lastPollAt = 0;
    uint32_t gapStartedAt = 0;
    uint32_t capturedAt = 0;
    uint16_t failedPolls = 0;
    bool enableAttempted = false;
    bool holdVerified = false;
    bool communicationGapObserved = false;
    bool afterGapCaptured = false;
    bool returnVerified = false;
    bool sonConsistentAfterGap = true;
    uint16_t returnSamples = 0;
    bool restoreAttempted = false;
    bool restoreAcknowledged = false;
    bool hasLatest = false;
    bool latestValid = false;
    Sample latest = {};
    Sample before = {};
    Sample after = {};
    Sample settledAfter = {};
};
struct Io {
    bool (*read)(uint16_t reg, uint16_t count, uint16_t* words);
    bool (*write)(uint16_t reg, uint16_t value);
};
const char* phaseName(Phase phase);
class Probe {
public:
    bool start(uint32_t now);
    void tick(uint32_t now, const Io& io);
    bool cancel(const Io& io);
    bool active() const;
    const View& view() const { return view_; }
private:
    bool sample(const Io& io, Sample& out);
    const char* invalid(const Sample& sample, uint16_t expectedSon) const;
    bool restore(const Io& io);
    void fail(const char* reason, const Io& io);
    View view_ = {};
    bool polled_ = false;
};
} // namespace kfm_zero_probe
