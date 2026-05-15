#pragma once

#include <Arduino.h>

struct RemoteVoiceResult {
    bool ok = false;
    int httpCode = 0;
    bool hasValue = false;
    int32_t value = 0;
    char command[24] = {};
    char transcript[128] = {};
    char message[96] = {};
};

void remote_voice_init();
String remote_voice_endpoint();
bool remote_voice_set_endpoint(const String& endpoint);
void remote_voice_clear_endpoint();
bool remote_voice_recognize(RemoteVoiceResult* result);
const char* remote_voice_detail();
