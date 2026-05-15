#include "remote_voice.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ctype.h>
#include <string.h>

#include "remote_audio.h"
#include "sg98_version.h"

namespace {

#ifndef REMOTE_VOICE_DEFAULT_URL
#define REMOTE_VOICE_DEFAULT_URL ""
#endif

#ifndef REMOTE_VOICE_CAPTURE_MS
#define REMOTE_VOICE_CAPTURE_MS 2200
#endif

constexpr char kPrefsNamespace[] = "remote-voice";
constexpr char kEndpointKey[] = "url";
constexpr uint32_t kPostTimeoutMs = 18000;

Preferences g_prefs;
bool g_prefsReady = false;
String g_endpoint;
char g_detail[96] = {};

void setDetail(const char* text) {
    if(text == nullptr) {
        g_detail[0] = '\0';
        return;
    }
    strncpy(g_detail, text, sizeof(g_detail) - 1);
    g_detail[sizeof(g_detail) - 1] = '\0';
}

void copyTo(char* dest, size_t destSize, const String& value) {
    if(dest == nullptr || destSize == 0) {
        return;
    }
    strncpy(dest, value.c_str(), destSize - 1);
    dest[destSize - 1] = '\0';
}

int jsonValueStart(const String& json, const char* key) {
    const String needle = String("\"") + key + "\"";
    int pos = json.indexOf(needle);
    if(pos < 0) {
        return -1;
    }
    pos = json.indexOf(':', pos + needle.length());
    if(pos < 0) {
        return -1;
    }
    ++pos;
    while(pos < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    return pos;
}

bool jsonStringValue(const String& json, const char* key, String* out) {
    if(out == nullptr) {
        return false;
    }
    int pos = jsonValueStart(json, key);
    if(pos < 0 || pos >= static_cast<int>(json.length()) || json[pos] != '"') {
        return false;
    }
    ++pos;
    String value;
    bool escaping = false;
    for(; pos < static_cast<int>(json.length()); ++pos) {
        const char ch = json[pos];
        if(escaping) {
            switch(ch) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += ch; break;
            }
            escaping = false;
            continue;
        }
        if(ch == '\\') {
            escaping = true;
            continue;
        }
        if(ch == '"') {
            *out = value;
            return true;
        }
        value += ch;
    }
    return false;
}

bool jsonIntValue(const String& json, const char* key, int32_t* out) {
    if(out == nullptr) {
        return false;
    }
    int pos = jsonValueStart(json, key);
    if(pos < 0) {
        return false;
    }
    char* endPtr = nullptr;
    const long parsed = strtol(json.c_str() + pos, &endPtr, 10);
    if(endPtr == nullptr || endPtr == json.c_str() + pos) {
        return false;
    }
    *out = static_cast<int32_t>(parsed);
    return true;
}

String parseCommandFromPayload(const String& payload, RemoteVoiceResult* result) {
    String command;
    if(jsonStringValue(payload, "command", &command) ||
       jsonStringValue(payload, "intent", &command) ||
       jsonStringValue(payload, "action", &command)) {
        String transcript;
        if(jsonStringValue(payload, "transcript", &transcript) ||
           jsonStringValue(payload, "text", &transcript)) {
            copyTo(result->transcript, sizeof(result->transcript), transcript);
        }
        int32_t value = 0;
        if(jsonIntValue(payload, "value", &value)) {
            result->hasValue = true;
            result->value = value;
        }
        command.trim();
        command.toLowerCase();
        return command;
    }

    command = payload;
    command.trim();
    command.toLowerCase();
    return command;
}

}  // namespace

void remote_voice_init() {
    if(!g_prefsReady) {
        g_prefsReady = g_prefs.begin(kPrefsNamespace, false);
    }
    if(g_prefsReady) {
        g_endpoint = g_prefs.getString(kEndpointKey, REMOTE_VOICE_DEFAULT_URL);
    } else {
        g_endpoint = REMOTE_VOICE_DEFAULT_URL;
    }
    setDetail(g_endpoint.length() > 0 ? "voice ready" : "voice endpoint unset");
}

String remote_voice_endpoint() {
    if(!g_prefsReady && g_endpoint.length() == 0) {
        remote_voice_init();
    }
    return g_endpoint;
}

bool remote_voice_set_endpoint(const String& endpoint) {
    String value = endpoint;
    value.trim();
    if(!value.startsWith("http://") && !value.startsWith("https://")) {
        setDetail("voice url must be http(s)");
        return false;
    }
    if(!g_prefsReady) {
        remote_voice_init();
    }
    g_endpoint = value;
    if(g_prefsReady) {
        g_prefs.putString(kEndpointKey, g_endpoint);
    }
    setDetail("voice endpoint saved");
    return true;
}

void remote_voice_clear_endpoint() {
    if(!g_prefsReady) {
        remote_voice_init();
    }
    g_endpoint = "";
    if(g_prefsReady) {
        g_prefs.remove(kEndpointKey);
    }
    setDetail("voice endpoint cleared");
}

bool remote_voice_recognize(RemoteVoiceResult* result) {
    if(result != nullptr) {
        *result = {};
    }
    if(result == nullptr) {
        setDetail("voice result missing");
        return false;
    }
    if(remote_voice_endpoint().length() == 0) {
        copyTo(result->message, sizeof(result->message), "voice endpoint unset");
        setDetail("voice endpoint unset");
        return false;
    }
    if(WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
        copyTo(result->message, sizeof(result->message), "wifi not connected");
        setDetail("wifi not connected");
        return false;
    }

    uint8_t* wav = nullptr;
    size_t wavSize = 0;
    if(!remote_audio_record_wav(REMOTE_VOICE_CAPTURE_MS, &wav, &wavSize)) {
        copyTo(result->message, sizeof(result->message), remote_audio_detail());
        setDetail(remote_audio_detail());
        return false;
    }

    HTTPClient http;
    http.setTimeout(kPostTimeoutMs);
    if(!http.begin(g_endpoint)) {
        remote_audio_free_recording(wav);
        copyTo(result->message, sizeof(result->message), "voice http begin failed");
        setDetail("voice http begin failed");
        return false;
    }
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("Accept", "application/json,text/plain");
    http.addHeader("X-SG98-Role", "remote");
    http.addHeader("X-SG98-Version", sg98_version::release());
    const int code = http.POST(wav, wavSize);
    remote_audio_free_recording(wav);
    result->httpCode = code;

    String payload;
    if(code > 0) {
        payload = http.getString();
    }
    http.end();

    if(code < 200 || code >= 300) {
        String message = code <= 0 ? String("voice http failed") : String("voice http ") + String(code);
        if(payload.length() > 0) {
            message += ": ";
            message += payload.substring(0, 64);
        }
        copyTo(result->message, sizeof(result->message), message);
        setDetail(result->message);
        return false;
    }

    const String command = parseCommandFromPayload(payload, result);
    if(command.length() == 0) {
        copyTo(result->message, sizeof(result->message), "voice returned no command");
        setDetail("voice returned no command");
        return false;
    }

    copyTo(result->command, sizeof(result->command), command);
    if(result->transcript[0] == '\0') {
        copyTo(result->transcript, sizeof(result->transcript), payload.substring(0, 120));
    }
    copyTo(result->message, sizeof(result->message), "ok");
    result->ok = true;
    setDetail("voice recognized");
    return true;
}

const char* remote_voice_detail() {
    return g_detail;
}
