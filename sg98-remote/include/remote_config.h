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
constexpr uint32_t kBatteryStatusPollMs = 10000;
constexpr uint32_t kBatteryPollMs = 60000;
constexpr uint32_t kWifiServiceMs = 75;
constexpr uint32_t kWebSocketServiceMs = 10;
constexpr uint32_t kWebSocketServiceIdleMs = 45;
constexpr uint32_t kTransportServiceMs = 75;
constexpr uint32_t kPollingServiceMs = 90;
constexpr uint32_t kTargetCommitDelayMs = 220;
constexpr uint32_t kPendingSyncRetryMs = 320;
constexpr uint32_t kPendingMenuPollMs = 320;
constexpr uint32_t kSafetyHeartbeatMs = 1000;
constexpr uint32_t kTransportSwitchDelayMs = 350;
constexpr uint32_t kTransportSearchWindowMs = 4500;
constexpr uint32_t kMdnsQueryMs = 3500;
constexpr uint32_t kWebSocketReconnectMs = 2000;
constexpr uint32_t kToastHoldMs = 2200;
constexpr uint32_t kDangerConfirmMs = 3000;
constexpr uint32_t kLinkTimeoutMs = 5000;
constexpr uint32_t kTapMaxDurationMs = 420;
constexpr bool kDisableWifiModemSleepForTesting = false;
constexpr bool kDisableIdleSleepForTesting = false;
constexpr uint32_t kIdleSleepAfterMs = 120000;
constexpr uint32_t kWakeToastMs = 900;
constexpr uint32_t kDockClockAfterMs = kIdleSleepAfterMs;
constexpr uint32_t kDockClockRedrawMs = 1000;
constexpr uint32_t kDockClockWeatherFetchMs = 30UL * 60UL * 1000UL;
constexpr uint32_t kDockClockNtpSyncMs = 60UL * 60UL * 1000UL;
constexpr const char* kDockClockLocationName = "San Jose";
constexpr float kDockClockWeatherLatitude = 37.3382f;
constexpr float kDockClockWeatherLongitude = -121.8863f;
constexpr const char* kDockClockTimezone = "PST8PDT,M3.2.0,M11.1.0";
constexpr const char* kDockClockNtpServer1 = "pool.ntp.org";
constexpr const char* kDockClockNtpServer2 = "time.nist.gov";
constexpr bool kDisplayAutoRotate = true;
constexpr uint32_t kDisplayAutoRotateSampleMs = 1000;

// Waveshare 1.75 header H2 exposes GPIO17/18/16; use GPIO17 as ADC2_CH6.
constexpr int8_t kPressureSensorAdcPin = 17;
constexpr uint16_t kPressureSensorZeroMillivolts = 400;
constexpr uint16_t kPressureSensorFullScaleMillivolts = 2400;
constexpr uint16_t kPressureSensorFullScaleCentiBar = 2000;
constexpr uint16_t kPressureProfilerGraphMaxCentiBar = 1600;
constexpr uint16_t kPressureProfilerSenseCentiBar = 50;
constexpr uint32_t kPressureProfilerSenseHoldMs = 450;
constexpr uint32_t kPressureProfilerWindowMs = 60000;
constexpr uint32_t kPressureProfilerVisibleWindowMs = 30000;
constexpr uint32_t kPressureProfilerSampleMs = 100;
constexpr uint32_t kPressureProfilerRedrawMs = 120;
constexpr uint32_t kPressureProfilerBleNotifyMs = 100;

}  // namespace remote_config
