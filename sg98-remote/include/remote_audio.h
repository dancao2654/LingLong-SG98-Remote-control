#pragma once

#include <Arduino.h>

enum class RemoteAudioTone : uint8_t {
    Click,
    SwipeLeft,
    SwipeRight,
    Sweep,
};

struct RemoteAudioStatus {
    bool supported = false;
    bool codecDetected = false;
    bool outputEnabled = false;
    bool i2sReady = false;
    bool lastPlayOk = false;
    uint32_t successfulPlays = 0;
    uint32_t failedPlays = 0;
    char detail[64] = {};
};

void remote_audio_init();
void remote_audio_service();
bool remote_audio_play(RemoteAudioTone tone);
RemoteAudioStatus remote_audio_status();
const char* remote_audio_detail();
