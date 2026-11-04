#pragma once
#include <cstddef>
#include <cstdint>
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE && defined(SG98_TSE_KFM_ALLOW_MOTION) && SG98_TSE_KFM_ALLOW_MOTION
#error Zero-preset audit must not be combined with motion-enabled firmware
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE && (!defined(SG98_TSE_KFM_COMMISSIONING) || !SG98_TSE_KFM_COMMISSIONING)
#error Zero-preset audit requires the commissioning transport gate
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE && ((!defined(SG98_TSE_KFM_COMMISSIONING) || !SG98_TSE_KFM_COMMISSIONING) || (defined(SG98_TSE_KFM_ALLOW_MOTION) && SG98_TSE_KFM_ALLOW_MOTION) || (defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE))
#error PA25 audit requires commissioning, no motion and no SON-enable probe
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE && ((!defined(SG98_TSE_KFM_COMMISSIONING) || !SG98_TSE_KFM_COMMISSIONING) || (defined(SG98_TSE_KFM_ALLOW_MOTION) && SG98_TSE_KFM_ALLOW_MOTION) || (defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE) || (defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE))
#error PA25 pulse requires isolated commissioning with normal motion and other probes disabled
#endif
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST && (!defined(SG98_TSE_KFM_PA25_PULSE) || !SG98_TSE_KFM_PA25_PULSE)
#error Touch test requires the isolated PA25 pulse transport
#endif
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE && ((!defined(SG98_TSE_KFM_COMMISSIONING) || !SG98_TSE_KFM_COMMISSIONING) || (!defined(SG98_TSE_KFM_ALLOW_MOTION) || !SG98_TSE_KFM_ALLOW_MOTION))
#error Run candidate requires commissioning transport and explicit motion opt-in
#endif
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE && (!defined(SG98_TSE_KFM_RUN_CANDIDATE) || !SG98_TSE_KFM_RUN_CANDIDATE)
#error Standalone candidate requires the guarded KFM run candidate
#endif
#ifndef SG98_TSE_KFM_MAX_RPM
#define SG98_TSE_KFM_MAX_RPM 500
#endif
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
static_assert(SG98_TSE_KFM_MAX_RPM >= 100 && SG98_TSE_KFM_MAX_RPM <= 2000,
              "KFM standalone candidate speed must be between100 and2000rpm");
#else
static_assert(SG98_TSE_KFM_MAX_RPM >= 100 && SG98_TSE_KFM_MAX_RPM <= 800,
              "KFM supervised test speed must be between100 and800rpm");
#endif
#ifndef SG98_TSE_KFM_OPERATION_MS
#define SG98_TSE_KFM_OPERATION_MS 30000
#endif
static_assert(SG98_TSE_KFM_OPERATION_MS >= 30000 && SG98_TSE_KFM_OPERATION_MS <= 300000,
              "KFM operation bound must be between30 and300seconds");
namespace kfm_commissioning {
#if defined(SG98_TSE_KFM_STANDALONE) && SG98_TSE_KFM_STANDALONE
constexpr bool standaloneCandidate = true;
#else
constexpr bool standaloneCandidate = false;
#endif
constexpr uint32_t maxOperationMs = SG98_TSE_KFM_OPERATION_MS;
constexpr int16_t maxRpm = SG98_TSE_KFM_MAX_RPM;
constexpr int16_t overspeedRpm = maxRpm + 150;
inline bool frameAllowed(const uint8_t* data, size_t size, uint8_t privateWindows = 0) {
#if !defined(SG98_TSE_KFM_COMMISSIONING) || !SG98_TSE_KFM_COMMISSIONING
    return true;
#else
    if(!data || size != 8 || data[0] != 1) return false;
    const uint16_t reg = (uint16_t(data[2]) << 8) | data[3];
    const uint16_t value = (uint16_t(data[4]) << 8) | data[5];
    if(data[1] == 3) {
        if(value == 0 || value > 16) return false;
        const uint32_t end = uint32_t(reg) + value - 1;
        return (reg <= 108 && end <= 108) ||
               (reg >= 0x100 && end <= 0x127) ||
               (reg >= 0x1000 && end <= 0x1016);
    }
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
    if(data[1] == 6 && reg == 0x99 && value == 0) return true;
    if((privateWindows & 8) && data[1] == 6 &&
       ((reg == 0x10F && value == 1) || (reg == 0x99 && static_cast<int16_t>(value) >= -maxRpm && static_cast<int16_t>(value) <= maxRpm))) return true;
    if(reg == 24) return false;
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
    if((privateWindows & 1) && data[1] == 6 && reg == 0x010F && value == 1) return true;
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
    if((privateWindows & 2) && data[1] == 6 && reg == 0x99 && value <= 1) return true;
#endif
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
    if((privateWindows & 4) && data[1] == 6 && ((reg == 0x10F && value == 1) || (reg == 0x99 && (value == 0 || value == static_cast<uint16_t>(-100))))) return true;
#endif
#if (defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE) || (defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE)
    if(reg == 24) return false; // no persistent PA writes in this audit image
#endif
    return data[1] == 6 && ((reg == 24 && value == 0) || (reg == 0x98 && value == 0) ||
                           (reg == 0x10F && value == 0) ||
                           (reg == 0x110 && value == 2));
#endif
}
}
