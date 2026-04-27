#include "smart_purge_learning.h"

#include "definitions.h"
#include "pdata.h"

namespace {
constexpr uint16_t kBucketCount = SIZEOFCALIBRATEARRAY / sizeof(uint16_t);
constexpr uint8_t kWindowSamples = 24;
constexpr uint16_t kUseConfidence = 96;
constexpr uint16_t kFullConfidence = 720;
constexpr uint16_t kMaxConfidence = 1200;
constexpr unsigned long kPersistIdleDelayMs = 4000;
constexpr unsigned long kPersistMinIntervalMs = 30000;

uint16_t learnedLoad[kBucketCount] = {0};
uint16_t learnedConfidence[kBucketCount] = {0};

uint8_t windowCount[kBucketCount] = {0};
uint32_t windowLoadSum[kBucketCount] = {0};
uint16_t windowLoadMin[kBucketCount] = {0};
uint16_t windowLoadMax[kBucketCount] = {0};

bool dirty = false;
unsigned long lastDirtyAt = 0;
unsigned long lastPersistAt = 0;

uint16_t abs_i16(int16_t value) {
    return value < 0 ? static_cast<uint16_t>(-value) : static_cast<uint16_t>(value);
}

uint16_t bucket_for_rpm(int16_t rpm) {
    uint16_t absRpm = abs_i16(rpm);
    uint16_t bucket = rpm_scalar == 0 ? 0 : absRpm / rpm_scalar;
    if(bucket >= kBucketCount) bucket = kBucketCount - 1;
    return bucket;
}

bool rpm_is_stable(int16_t targetRpm, int16_t currentRpm) {
    const uint16_t target = abs_i16(targetRpm);
    const uint16_t current = abs_i16(currentRpm);
    if(target < 100 || current < 100) return false;

    const uint16_t tolerance = target / 10 > 75 ? target / 10 : 75;
    const uint16_t diff = target > current ? target - current : current - target;
    return diff <= tolerance;
}

uint16_t minimum_learn_load(uint16_t baselineTorque) {
    const uint16_t percentFloor = baselineTorque / 5;
    return percentFloor > 3 ? percentFloor : 3;
}

bool load_sample_is_reasonable(uint16_t load, uint16_t baselineTorque) {
    const uint16_t minLoad = minimum_learn_load(baselineTorque);
    if(load < minLoad) return false;

    // Hard stalls and bean snaps can create single-sample spikes. Keep those out
    // of the slow learner so the learned threshold follows behavior, not noise.
    const uint32_t maxLoad = static_cast<uint32_t>(baselineTorque) * 5U + 80U;
    return load <= maxLoad;
}

void reset_window(uint16_t bucket) {
    windowCount[bucket] = 0;
    windowLoadSum[bucket] = 0;
    windowLoadMin[bucket] = 0;
    windowLoadMax[bucket] = 0;
}

void commit_window(uint16_t bucket) {
    const uint8_t count = windowCount[bucket];
    if(count < kWindowSamples) return;

    uint32_t sum = windowLoadSum[bucket];
    if(count > 2) {
        sum -= windowLoadMin[bucket];
        sum -= windowLoadMax[bucket];
    }

    const uint16_t divisor = count > 2 ? count - 2 : count;
    const uint16_t trimmedLoad = static_cast<uint16_t>(sum / divisor);
    const uint16_t oldLoad = learnedLoad[bucket];

    if(oldLoad == 0) {
        learnedLoad[bucket] = trimmedLoad;
    } else {
        const int32_t delta = static_cast<int32_t>(trimmedLoad) - static_cast<int32_t>(oldLoad);
        int32_t step = delta / 16;
        if(step == 0 && delta != 0) step = delta > 0 ? 1 : -1;

        int32_t updated = static_cast<int32_t>(oldLoad) + step;
        if(updated < 0) updated = 0;
        learnedLoad[bucket] = static_cast<uint16_t>(updated);
    }

    const uint32_t confidence = static_cast<uint32_t>(learnedConfidence[bucket]) + count;
    learnedConfidence[bucket] = confidence > kMaxConfidence ? kMaxConfidence : static_cast<uint16_t>(confidence);
    dirty = true;
    lastDirtyAt = millis();
}
}

void smart_purge_learning_init() {
    pdata_read_smart_purge_learning(learnedLoad, learnedConfidence, kBucketCount);
    smart_purge_learning_reset_runtime();
}

void smart_purge_learning_reset_runtime() {
    for(uint16_t i = 0; i < kBucketCount; i++) {
        reset_window(i);
    }
}

void smart_purge_learning_note_sample(int16_t targetRpm,
                                      int16_t currentRpm,
                                      int16_t torque,
                                      uint16_t baselineTorque,
                                      uint16_t manualThreshold) {
    if(!rpm_is_stable(targetRpm, currentRpm)) return;
    if(torque <= static_cast<int16_t>(baselineTorque)) return;

    const uint16_t bucket = bucket_for_rpm(targetRpm);
    const uint16_t load = static_cast<uint16_t>(torque - baselineTorque);
    if(!load_sample_is_reasonable(load, baselineTorque)) return;

    // If the manual threshold is already much higher than this sample, treat it
    // as doubtful no-load behavior. Learning still starts a little below manual
    // threshold so it can gradually correct a threshold that was set too high.
    const uint16_t softGate = manualThreshold > baselineTorque
                                  ? baselineTorque + ((manualThreshold - baselineTorque) * 7U) / 10U
                                  : baselineTorque + minimum_learn_load(baselineTorque);
    if(torque < static_cast<int16_t>(softGate)) return;

    if(windowCount[bucket] == 0) {
        windowLoadMin[bucket] = load;
        windowLoadMax[bucket] = load;
    } else {
        if(load < windowLoadMin[bucket]) windowLoadMin[bucket] = load;
        if(load > windowLoadMax[bucket]) windowLoadMax[bucket] = load;
    }

    windowLoadSum[bucket] += load;
    windowCount[bucket]++;

    if(windowCount[bucket] >= kWindowSamples) {
        commit_window(bucket);
        reset_window(bucket);
    }
}

uint16_t smart_purge_learning_threshold(int16_t targetRpm,
                                        uint16_t baselineTorque,
                                        uint16_t manualThreshold) {
    const uint16_t bucket = bucket_for_rpm(targetRpm);
    const uint16_t confidence = learnedConfidence[bucket];
    const uint16_t load = learnedLoad[bucket];

    if(load == 0 || confidence < kUseConfidence) return manualThreshold;

    const uint32_t learnedThreshold = static_cast<uint32_t>(baselineTorque) + (static_cast<uint32_t>(load) * 45U) / 100U;
    const int32_t delta = static_cast<int32_t>(learnedThreshold) - static_cast<int32_t>(manualThreshold);
    const uint16_t confidenceSpan = kFullConfidence - kUseConfidence;
    const uint16_t confidenceWeight = confidence >= kFullConfidence
                                          ? 1000
                                          : static_cast<uint16_t>(((confidence - kUseConfidence) * 1000U) / confidenceSpan);
    int32_t blended = static_cast<int32_t>(manualThreshold) + ((delta * confidenceWeight) / 1000);

    const uint16_t learnedLower = baselineTorque + minimum_learn_load(baselineTorque);
    const uint16_t learnedUpper = baselineTorque + static_cast<uint16_t>((static_cast<uint32_t>(load) * 75U) / 100U);
    const uint16_t lower = manualThreshold < learnedLower ? manualThreshold : learnedLower;
    const uint16_t upper = manualThreshold > learnedUpper ? manualThreshold : learnedUpper;
    if(blended < lower) blended = lower;
    if(blended > upper) blended = upper;

    return static_cast<uint16_t>(blended);
}

void smart_purge_learning_service_persist(bool idleState, unsigned long lastActivityAt) {
    if(!dirty || !idleState) return;

    const unsigned long now = millis();
    if(now - lastDirtyAt < kPersistIdleDelayMs) return;
    if(now - lastActivityAt < kPersistIdleDelayMs) return;
    if(lastPersistAt != 0 && now - lastPersistAt < kPersistMinIntervalMs) return;

    pdata_write_smart_purge_learning(learnedLoad, learnedConfidence, kBucketCount);
    dirty = false;
    lastPersistAt = now;
}

size_t smart_purge_learning_bucket_count() {
    return kBucketCount;
}
