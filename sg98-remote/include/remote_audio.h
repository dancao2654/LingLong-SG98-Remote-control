#pragma once

#include <Arduino.h>

enum class RemoteAudioTone : uint8_t {
    Click,
    SwipeLeft,
    SwipeRight,
    Sweep,
    Reveal,
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
void remote_audio_cancel_reveal();
bool remote_audio_record_wav(uint32_t durationMs, uint8_t** outBuffer, size_t* outSize);
void remote_audio_free_recording(uint8_t* buffer);
RemoteAudioStatus remote_audio_status();
const char* remote_audio_detail();
