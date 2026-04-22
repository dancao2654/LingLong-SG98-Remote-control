#pragma once

#include <stdint.h>

namespace remote_config {

constexpr uint8_t kEspNowChannel = 1;
constexpr uint16_t kWebSocketPort = 81;

// Replace this with the SG98 controller board's STA MAC address.
constexpr uint8_t kTargetMac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr bool kTargetMacConfigured = false;

constexpr uint32_t kStatusPollIdleMs = 450;
constexpr uint32_t kStatusPollInteractiveMs = 120;
constexpr uint32_t kStatusPollRunningMs = 90;
constexpr uint32_t kStatusPollRoundInputMs = 360;
constexpr uint32_t kStatusPollRoundMenuMs = 900;
constexpr uint32_t kStatusPollBoostMs = 1800;
constexpr uint32_t kVisibleMenuPollMs = 1800;
constexpr uint32_t kBackgroundMenuPollMs = 6000;
constexpr uint32_t kBatteryPollMs = 5000;
constexpr uint32_t kWifiServiceMs = 75;
constexpr uint32_t kWebSocketServiceMs = 10;
constexpr uint32_t kWebSocketServiceIdleMs = 45;
constexpr uint32_t kTransportServiceMs = 75;
constexpr uint32_t kPollingServiceMs = 90;
constexpr uint32_t kTargetCommitDelayMs = 220;
constexpr uint32_t kPendingSyncRetryMs = 320;
constexpr uint32_t kPendingMenuPollMs = 320;
constexpr uint32_t kTransportSwitchDelayMs = 350;
constexpr uint32_t kTransportSearchWindowMs = 4500;
constexpr uint32_t kMdnsQueryMs = 3500;
constexpr uint32_t kWebSocketReconnectMs = 2000;
constexpr uint32_t kToastHoldMs = 2200;
constexpr uint32_t kDangerConfirmMs = 3000;
constexpr uint32_t kLinkTimeoutMs = 5000;
constexpr uint32_t kTapMaxDurationMs = 420;
constexpr bool kDisableWifiModemSleepForTesting = true;
constexpr bool kDisableIdleSleepForTesting = true;
constexpr uint32_t kIdleSleepAfterMs = 90000;
constexpr uint32_t kWakeToastMs = 900;

}  // namespace remote_config
