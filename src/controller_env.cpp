#include "controller_env.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include "main.h"
#include "sg98_ota_update.h"
#include "wifi_ui.h"

namespace {
constexpr uint32_t kValidUnixTime = 1700000000UL;
constexpr uint32_t kNtpSyncMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kWeatherFetchMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kWeatherRetryMs = 60UL * 1000UL;
constexpr uint16_t kWeatherHttpTimeoutMs = 1200;
constexpr uint32_t kNetworkFetchMinFreeHeap = 30000;
constexpr uint32_t kFirmwareCheckUserQuietMs = 5000;
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
sg98_protocol::EnvironmentFrame g_environment;
sg98_ota_update::State g_otaUpdate;

bool controllerCanUseBackgroundNetwork() {
    return state == IDLE || state == IDLE_GBW || controller_is_menu_state(state);
}

bool controllerCanCheckFirmware() {
    return (state == IDLE || state == IDLE_GBW) &&
           static_cast<uint32_t>(millis() - lastActivity) >= kFirmwareCheckUserQuietMs;
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

    (void)sg98_ota_update::checkLatestManifest(&g_otaUpdate, now);
}

void refreshTimeSnapshot() {
    const time_t unixNow = time(nullptr);
    g_environment.timeValid = unixNow >= static_cast<time_t>(kValidUnixTime);
    g_environment.unixTime = g_environment.timeValid ? static_cast<uint32_t>(unixNow) : 0;
}
}  // namespace

void controller_env_poll() {
    if(!wifi_ui_sta_connected()) {
        return;
    }

    const uint32_t now = millis();
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
