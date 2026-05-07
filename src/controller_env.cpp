#include "controller_env.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include "controller_board.h"
#include "controller_ota.h"
#include "espnow_control.h"
#include "main.h"
#include "sg98_ota_update.h"
#include "sg98_ota_source.h"
#include "wifi_ui.h"

namespace {
constexpr uint32_t kValidUnixTime = 1700000000UL;
constexpr uint32_t kNtpSyncMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kWeatherFetchMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kWeatherRetryMs = 60UL * 1000UL;
constexpr uint16_t kWeatherHttpTimeoutMs = 1200;
constexpr uint32_t kNetworkFetchMinFreeHeap = 30000;
constexpr uint32_t kFirmwareCheckUserQuietMs = 5000;
constexpr uint32_t kAutoOtaFirstCheckDelayMs = sg98_ota_update::kOnlineCheckIntervalMs;
constexpr uint32_t kAutoOtaRetryAfterFailureMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kAutoOtaRetryAfterNoSavedWifiMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kAutoOtaWifiConnectTimeoutMs = 30000UL;
constexpr uint32_t kAutoOtaRemoteQuietMs = 10UL * 60UL * 1000UL;
constexpr const char* kTimezone = "PST8PDT,M3.2.0,M11.1.0";
constexpr const char* kNtpServer1 = "pool.ntp.org";
constexpr const char* kNtpServer2 = "time.nist.gov";
constexpr const char* kLocationName = "San Jose";
constexpr float kWeatherLatitude = 37.3382f;
constexpr float kWeatherLongitude = -121.8863f;

bool g_timeConfigured = false;
uint32_t g_lastNtpSyncAt = 0;
uint32_t g_lastWeatherAttemptAt = 0;
uint32_t g_lastWeatherFetchAt = 0;
uint32_t g_autoOtaNextAttemptAt = 0;
sg98_protocol::EnvironmentFrame g_environment;
sg98_ota_update::State g_otaUpdate;

struct AutoOtaWifiGuard {
    bool wifiWasActive = false;
    bool espNowWasActive = false;
};

bool timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void scheduleAutoOtaCheck(uint32_t now, uint32_t delayMs) {
    g_autoOtaNextAttemptAt = now + delayMs;
}

bool controllerCanUseBackgroundNetwork() {
    return state == IDLE || state == IDLE_GBW || controller_is_menu_state(state);
}

bool controllerCanCheckFirmware() {
    return (state == IDLE || state == IDLE_GBW) &&
           static_cast<uint32_t>(millis() - lastActivity) >= kFirmwareCheckUserQuietMs;
}

bool autoOtaRemoteQuiet(uint32_t now) {
    if(!espnow_control_is_active()) {
        return true;
    }
    const unsigned long lastRxAt = espnow_control_last_rx_at();
    if(lastRxAt != 0 && static_cast<uint32_t>(now - lastRxAt) < kAutoOtaRemoteQuietMs) {
        return false;
    }
    const unsigned long lastCommandAt = espnow_control_last_command_rx_at();
    if(lastCommandAt != 0 && static_cast<uint32_t>(now - lastCommandAt) < kAutoOtaRemoteQuietMs) {
        return false;
    }
    return true;
}

bool jsonNumberBetween(const String& json, const char* key, int start, int end, float* out) {
    if(key == nullptr || out == nullptr || start < 0) {
        return false;
    }
    const int boundedEnd = end < 0 ? static_cast<int>(json.length()) : end;
    int pos = json.indexOf(key, static_cast<unsigned int>(start));
    if(pos < 0 || pos >= boundedEnd) {
        return false;
    }
    pos += static_cast<int>(strlen(key));
    while(pos < boundedEnd && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '[')) {
        ++pos;
    }

    char buffer[20] = {};
    uint8_t len = 0;
    while(pos < boundedEnd && len + 1 < sizeof(buffer)) {
        const char c = json[pos];
        if((c >= '0' && c <= '9') || c == '-' || c == '+') {
            buffer[len++] = c;
        } else if(c == '.') {
            buffer[len++] = c;
        } else {
            break;
        }
        ++pos;
    }
    if(len == 0) {
        return false;
    }
    *out = static_cast<float>(atof(buffer));
    return true;
}

bool updateWeatherFromJson(const String& payload) {
    const int currentStart = payload.indexOf("\"current\":");
    const int hourlyStart = payload.indexOf("\"hourly\":");
    if(currentStart < 0) {
        return false;
    }

    float temperature = 0.0f;
    float weatherCode = 0.0f;
    if(!jsonNumberBetween(payload, "\"temperature_2m\":", currentStart, hourlyStart, &temperature) ||
       !jsonNumberBetween(payload, "\"weather_code\":", currentStart, hourlyStart, &weatherCode)) {
        return false;
    }

    float precip = -1.0f;
    if(hourlyStart >= 0) {
        (void)jsonNumberBetween(payload, "\"precipitation_probability\":[", hourlyStart, -1, &precip);
    }

    g_environment.weatherValid = true;
    g_environment.temperatureF = static_cast<int16_t>(lroundf(temperature));
    g_environment.weatherCode = static_cast<uint16_t>(max<int>(0, static_cast<int>(lroundf(weatherCode))));
    g_environment.precipitationPercent =
        precip >= 0.0f ? static_cast<int8_t>(constrain(static_cast<int>(lroundf(precip)), 0, 100)) : -1;
    g_environment.location = kLocationName;
    return true;
}

void restoreAutoOtaWifi(const AutoOtaWifiGuard& guard) {
    if(guard.wifiWasActive) {
        return;
    }
    wifi_ui_stop();
    if(guard.espNowWasActive && !espnow_control_is_active()) {
        (void)espnow_control_start(false);
    }
}

bool ensureAutoOtaWifi(AutoOtaWifiGuard* guard) {
    if(guard == nullptr) {
        return false;
    }
    guard->wifiWasActive = wifi_ui_is_active();
    guard->espNowWasActive = espnow_control_is_active();

    if(wifi_ui_sta_connected()) {
        return true;
    }
    if(!wifi_ui_has_saved_network()) {
        return false;
    }

    if(!guard->wifiWasActive && guard->espNowWasActive) {
        espnow_control_stop();
    }
    if(!wifi_ui_start()) {
        restoreAutoOtaWifi(*guard);
        return false;
    }

    const uint32_t startedAt = millis();
    while(static_cast<uint32_t>(millis() - startedAt) < kAutoOtaWifiConnectTimeoutMs) {
        wifi_ui_poll();
        if(wifi_ui_sta_connected()) {
            return true;
        }
        delay(50);
        yield();
    }

    restoreAutoOtaWifi(*guard);
    return false;
}

String weatherUrl() {
    String url = "http://api.open-meteo.com/v1/forecast?latitude=";
    url += String(kWeatherLatitude, 4);
    url += "&longitude=";
    url += String(kWeatherLongitude, 4);
    url += "&current=temperature_2m,weather_code,is_day";
    url += "&hourly=temperature_2m,precipitation_probability,weather_code";
    url += "&temperature_unit=fahrenheit&forecast_hours=6&timezone=auto";
    return url;
}

void serviceTime(uint32_t now) {
    if(g_timeConfigured && (now - g_lastNtpSyncAt) < kNtpSyncMs) {
        return;
    }
    configTzTime(kTimezone, kNtpServer1, kNtpServer2);
    g_timeConfigured = true;
    g_lastNtpSyncAt = now;
}

void serviceWeather(uint32_t now) {
    if(!controllerCanUseBackgroundNetwork()) {
        return;
    }
    if(wifi_ui_web_activity_recent() || ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        return;
    }
    const uint32_t interval = g_environment.weatherValid ? kWeatherFetchMs : kWeatherRetryMs;
    if(g_lastWeatherAttemptAt != 0 && (now - g_lastWeatherAttemptAt) < interval) {
        return;
    }
    g_lastWeatherAttemptAt = now;

    HTTPClient http;
    http.setTimeout(kWeatherHttpTimeoutMs);
    if(!http.begin(weatherUrl())) {
        return;
    }
    const int code = http.GET();
    if(code == HTTP_CODE_OK) {
        const String payload = http.getString();
        if(updateWeatherFromJson(payload)) {
            g_lastWeatherFetchAt = now;
        }
    }
    http.end();
}

String otaCheckMessage() {
    if(g_otaUpdate.checked) {
        return g_otaUpdate.updateAvailable
                   ? String("update available ") + sg98_ota_update::latestLabel(g_otaUpdate)
                   : String("current");
    }
    return g_otaUpdate.message.length() > 0 ? g_otaUpdate.message : String("check failed");
}

bool downloadCheckedManifestSpec(const sg98_ota_download::ManifestSpec& spec, String* message) {
    if(controller_ota_ready_for_remote()) {
        if(controller_ota_ready_matches(spec)) {
            if(message != nullptr) {
                *message = String("download ready ") + sg98_ota_update::latestLabel(g_otaUpdate);
            }
            return true;
        }
        controller_ota_clear_ready();
    }

    sg98_ota_download::DownloadOptions options;
    options.expectedSize = spec.size;
    options.expectedSha256 = spec.sha256;
    options.version = spec.version;

    sg98_ota_download::Result downloadResult;
    const bool downloaded = sg98_ota_download::downloadToNextSlot(spec.imageUrl, options, &downloadResult);
    if(downloaded) {
        (void)controller_ota_note_downloaded(downloadResult);
        g_otaUpdate.message = "download ready";
        if(message != nullptr) {
            *message = String("download ready ") + sg98_ota_update::latestLabel(g_otaUpdate);
        }
    } else {
        g_otaUpdate.message = String("download failed: ") + downloadResult.message;
        if(message != nullptr) {
            *message = g_otaUpdate.message;
        }
    }
    return downloaded;
}

bool performFirmwareUpdateCheck(uint32_t now, bool allowControllerDownload) {
    sg98_ota_download::ManifestSpec spec;
    if(!sg98_ota_update::checkLatestManifest(&g_otaUpdate, now, &spec)) {
        return false;
    }

    if(!allowControllerDownload ||
       !g_otaUpdate.updateAvailable ||
       !controller_board_requires_remote_recovery()) {
        return true;
    }

    return downloadCheckedManifestSpec(spec, nullptr);
}

void serviceFirmwareUpdateCheck(uint32_t now) {
    if(!controllerCanCheckFirmware()) {
        return;
    }
    if(wifi_ui_web_activity_recent() || ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        return;
    }
    if(!g_environment.timeValid && !(g_environment.weatherValid && g_lastWeatherFetchAt != 0)) {
        return;
    }
    if(!sg98_ota_update::shouldCheck(g_otaUpdate, now)) {
        return;
    }

    (void)performFirmwareUpdateCheck(now, false);
}

void serviceAutoFirmwareUpdateWindow(uint32_t now) {
    if(g_autoOtaNextAttemptAt == 0) {
        scheduleAutoOtaCheck(now, kAutoOtaFirstCheckDelayMs);
        return;
    }
    if(!timeReached(now, g_autoOtaNextAttemptAt)) {
        return;
    }
    if(wifi_ui_sta_connected() || !controllerCanCheckFirmware() || !autoOtaRemoteQuiet(now)) {
        return;
    }
    if(wifi_ui_web_activity_recent() || ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        return;
    }
    if(!sg98_ota_update::shouldCheck(g_otaUpdate, now)) {
        scheduleAutoOtaCheck(now, sg98_ota_update::kOnlineCheckIntervalMs);
        return;
    }
    if(!wifi_ui_has_saved_network()) {
        scheduleAutoOtaCheck(now, kAutoOtaRetryAfterNoSavedWifiMs);
        return;
    }

    AutoOtaWifiGuard guard;
    if(!ensureAutoOtaWifi(&guard)) {
        scheduleAutoOtaCheck(now, kAutoOtaRetryAfterFailureMs);
        return;
    }

    const uint32_t checkAt = millis();
    const bool ok = performFirmwareUpdateCheck(checkAt, true);
    restoreAutoOtaWifi(guard);
    scheduleAutoOtaCheck(millis(), ok ? sg98_ota_update::kOnlineCheckIntervalMs
                                      : kAutoOtaRetryAfterFailureMs);
}

void refreshTimeSnapshot() {
    const time_t unixNow = time(nullptr);
    g_environment.timeValid = unixNow >= static_cast<time_t>(kValidUnixTime);
    g_environment.unixTime = g_environment.timeValid ? static_cast<uint32_t>(unixNow) : 0;
}
}  // namespace

void controller_env_poll() {
    const uint32_t now = millis();
    if(!wifi_ui_sta_connected()) {
        serviceAutoFirmwareUpdateWindow(now);
        return;
    }

    serviceTime(now);
    refreshTimeSnapshot();
    if(wifi_ui_web_activity_recent()) {
        return;
    }
    serviceWeather(now);
    serviceFirmwareUpdateCheck(now);
}

String controller_env_build_frame() {
    refreshTimeSnapshot();
    return sg98_protocol::environment(g_environment);
}

bool controller_env_apply_peer_frame(const sg98_protocol::EnvironmentFrame& frame) {
    const uint32_t now = millis();
    bool applied = false;

    if(frame.timeValid && frame.unixTime >= kValidUnixTime && !controller_env_time_valid()) {
        setenv("TZ", kTimezone, 1);
        tzset();
        timeval tv = {};
        tv.tv_sec = static_cast<time_t>(frame.unixTime);
        settimeofday(&tv, nullptr);
        g_timeConfigured = true;
        g_lastNtpSyncAt = now;
        refreshTimeSnapshot();
        applied = true;
    }

    if(frame.weatherValid && !controller_env_weather_valid()) {
        g_environment.weatherValid = true;
        g_environment.temperatureF = frame.temperatureF;
        g_environment.weatherCode = frame.weatherCode;
        g_environment.precipitationPercent = frame.precipitationPercent;
        g_environment.location = frame.location.length() > 0 ? frame.location : String(kLocationName);
        g_lastWeatherFetchAt = now;
        g_lastWeatherAttemptAt = now;
        applied = true;
    }

    return applied;
}

bool controller_env_time_valid() {
    refreshTimeSnapshot();
    return g_environment.timeValid;
}

bool controller_env_weather_valid() {
    return g_environment.weatherValid && g_lastWeatherFetchAt != 0;
}

uint32_t controller_env_last_ntp_sync_ms() {
    return controller_env_time_valid() ? g_lastNtpSyncAt : 0;
}

uint32_t controller_env_last_weather_fetch_ms() {
    return controller_env_weather_valid() ? g_lastWeatherFetchAt : 0;
}

String controller_env_location() {
    return g_environment.location;
}

int16_t controller_env_temperature_f() {
    return g_environment.temperatureF;
}

int8_t controller_env_precipitation_percent() {
    return g_environment.precipitationPercent;
}

String controller_env_ota_update_line() {
    return sg98_ota_update::statusLine(g_otaUpdate);
}

String controller_env_ota_update_detail() {
    return sg98_ota_update::detailLine(g_otaUpdate);
}

bool controller_env_ota_update_available() {
    return g_otaUpdate.checked && g_otaUpdate.updateAvailable;
}

uint32_t controller_env_last_ota_check_ms() {
    return g_otaUpdate.lastAttemptAt;
}

bool controller_env_check_ota_manifest_now(const String& manifestUrl, String* message) {
    if(ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        if(message != nullptr) {
            *message = "low heap";
        }
        return false;
    }

    const String resolvedUrl = sg98_ota_source::resolveManifestUrl(manifestUrl);
    if(resolvedUrl.length() == 0) {
        if(message != nullptr) {
            *message = "missing manifest url";
        }
        return false;
    }

    sg98_ota_download::ManifestSpec spec;
    const bool ok = sg98_ota_update::checkManifestUrl(&g_otaUpdate, millis(), resolvedUrl, &spec);
    if(message != nullptr) {
        *message = ok ? otaCheckMessage() : g_otaUpdate.message;
    }
    return ok;
}

bool controller_env_download_ota_if_available_now(const String& manifestUrl, String* message) {
    if(ESP.getFreeHeap() < kNetworkFetchMinFreeHeap) {
        if(message != nullptr) {
            *message = "low heap";
        }
        return false;
    }

    const String resolvedUrl = sg98_ota_source::resolveManifestUrl(manifestUrl);
    if(resolvedUrl.length() == 0) {
        if(message != nullptr) {
            *message = "missing manifest url";
        }
        return false;
    }

    sg98_ota_download::ManifestSpec spec;
    if(!sg98_ota_update::checkManifestUrl(&g_otaUpdate, millis(), resolvedUrl, &spec)) {
        if(message != nullptr) {
            *message = g_otaUpdate.message;
        }
        return false;
    }
    if(!g_otaUpdate.updateAvailable) {
        if(message != nullptr) {
            *message = "current";
        }
        return true;
    }

    return downloadCheckedManifestSpec(spec, message);
}
