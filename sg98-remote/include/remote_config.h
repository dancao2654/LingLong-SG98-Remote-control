#pragma once

#include <stdint.h>

namespace remote_config {

constexpr uint8_t kEspNowChannel = 1;

// Replace this with the SG98 controller board's STA MAC address.
constexpr uint8_t kTargetMac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr bool kTargetMacConfigured = false;

constexpr uint32_t kStatusPollMs = 450;
constexpr uint32_t kVisibleMenuPollMs = 1800;
constexpr uint32_t kBackgroundMenuPollMs = 6000;
constexpr uint32_t kBatteryPollMs = 5000;
constexpr uint32_t kTargetCommitDelayMs = 220;
constexpr uint32_t kToastHoldMs = 2200;
constexpr uint32_t kDangerConfirmMs = 3000;
constexpr uint32_t kLinkTimeoutMs = 2200;
constexpr uint32_t kTapMaxDurationMs = 420;
constexpr uint32_t kIdleSleepAfterMs = 90000;
constexpr uint32_t kWakeToastMs = 900;

}  // namespace remote_config
