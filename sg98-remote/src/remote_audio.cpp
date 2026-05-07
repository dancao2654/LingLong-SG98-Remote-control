#include "remote_audio.h"

#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include "remote_board.h"

namespace {

constexpr uint8_t kEs8311Addr = 0x18;
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kMclkHz = kSampleRate * 256;
constexpr uint16_t kAmplitude = 1800;
constexpr uint8_t kCodecVolume = 90;
constexpr uint8_t kCodecMicGain = 3;
constexpr uint32_t kPrepareRetryMs = 1500;

RemoteAudioStatus g_status;
bool g_initialized = false;
bool g_codecConfigured = false;
uint32_t g_lastPrepareAttemptAt = 0;
I2SClass g_audioI2s;

void setDetail(const char* text) {
    if(text == nullptr) {
        g_status.detail[0] = '\0';
        return;
    }
    strncpy(g_status.detail, text, sizeof(g_status.detail) - 1);
    g_status.detail[sizeof(g_status.detail) - 1] = '\0';
}

bool isSupportedBoard() {
    const RemoteBoardPins& pins = remote_board_pins();
    return pins.audioBclk >= 0 && pins.audioLrclk >= 0 &&
           pins.audioDout >= 0 && pins.audioMclk >= 0;
}

void enableOutputStage() {
    const RemoteBoardPins& pins = remote_board_pins();
    if(pins.audioEnable >= 0) {
        pinMode(pins.audioEnable, OUTPUT);
        digitalWrite(pins.audioEnable, HIGH);
        g_status.outputEnabled = true;
    }
}

void resetAudioTransportState() {
    if(g_status.i2sReady) {
        g_audioI2s.end();
    }
    g_status.i2sReady = false;
    g_status.codecDetected = false;
    g_codecConfigured = false;
}

TwoWire* audioWire() {
    TwoWire* bus = remote_board_services().i2c;
    return bus != nullptr ? bus : &Wire;
}

bool codecWrite(uint8_t reg, uint8_t value) {
    TwoWire* bus = audioWire();
    bus->beginTransmission(kEs8311Addr);
    bus->write(reg);
    bus->write(value);
    return bus->endTransmission() == 0;
}

bool codecRead(uint8_t reg, uint8_t* value) {
    if(value == nullptr) {
        return false;
    }

    TwoWire* bus = audioWire();
    bus->beginTransmission(kEs8311Addr);
    bus->write(reg);
    if(bus->endTransmission(false) != 0) {
        return false;
    }
    if(bus->requestFrom(static_cast<int>(kEs8311Addr), 1) != 1) {
        return false;
    }

    *value = static_cast<uint8_t>(bus->read());
    return true;
}

bool probeCodec() {
    TwoWire* bus = audioWire();
    bus->beginTransmission(kEs8311Addr);
    return bus->endTransmission() == 0;
}

bool configureCodec() {
    if(g_codecConfigured) {
        return true;
    }
    if(!probeCodec()) {
        setDetail("codec not found on i2c");
        return false;
    }

    uint8_t reg06 = 0;
    if(!codecWrite(0x00, 0x1F)) {
        setDetail("codec reset write failed");
        return false;
    }
    delay(20);
    if(!codecWrite(0x00, 0x00) || !codecWrite(0x00, 0x80)) {
        setDetail("codec reset sequence failed");
        return false;
    }

    if(!codecWrite(0x01, 0x3F)) {
        setDetail("codec clk reg01 failed");
        return false;
    }
    if(!codecRead(0x06, &reg06)) {
        setDetail("codec clk reg06 read failed");
        return false;
    }
    reg06 &= 0xE0;
    reg06 |= 0x03;
    if(!codecWrite(0x02, 0x00) ||
       !codecWrite(0x03, 0x10) ||
       !codecWrite(0x04, 0x10) ||
       !codecWrite(0x05, 0x00) ||
       !codecWrite(0x06, reg06) ||
       !codecWrite(0x07, 0x00) ||
       !codecWrite(0x08, 0xFF)) {
        setDetail("codec clock config failed");
        return false;
    }

    if(!codecRead(0x00, &reg06)) {
        setDetail("codec fmt read failed");
        return false;
    }
    reg06 &= 0xBF;
    if(!codecWrite(0x00, reg06) ||
       !codecWrite(0x09, 0x0C) ||
       !codecWrite(0x0A, 0x0C)) {
        setDetail("codec format config failed");
        return false;
    }

    if(!codecWrite(0x0D, 0x01) ||
       !codecWrite(0x0E, 0x02) ||
       !codecWrite(0x12, 0x00) ||
       !codecWrite(0x13, 0x10) ||
       !codecWrite(0x1C, 0x6A) ||
       !codecWrite(0x37, 0x08) ||
       !codecWrite(0x17, 0xC8) ||
       !codecWrite(0x14, 0x1A) ||
       !codecWrite(0x16, kCodecMicGain) ||
       !codecWrite(0x31, 0x00)) {
        setDetail("codec analog path failed");
        return false;
    }

    const uint8_t volumeReg = kCodecVolume == 0 ? 0 : static_cast<uint8_t>(((kCodecVolume * 256) / 100) - 1);
    if(!codecWrite(0x32, volumeReg)) {
        setDetail("codec volume failed");
        return false;
    }

    g_status.codecDetected = true;
    g_codecConfigured = true;
    setDetail("codec configured");
    return true;
}

bool ensureI2sReady() {
    if(g_status.i2sReady) {
        return true;
    }

    const RemoteBoardPins& pins = remote_board_pins();
    if(pins.audioBclk < 0 || pins.audioLrclk < 0 || pins.audioDout < 0 || pins.audioMclk < 0) {
        setDetail("audio pins missing");
        return false;
    }

    g_audioI2s.end();
    g_audioI2s.setPins(pins.audioBclk, pins.audioLrclk, pins.audioDout, pins.audioDin, pins.audioMclk);
    if(!g_audioI2s.begin(I2S_MODE_STD,
                         kSampleRate,
                         I2S_DATA_BIT_WIDTH_16BIT,
                         I2S_SLOT_MODE_STEREO,
                         I2S_STD_SLOT_BOTH)) {
        setDetail("I2S begin failed");
        return false;
    }
    g_status.i2sReady = true;
    return true;
}

bool writeTone(float startHz, float endHz, uint32_t durationMs) {
    if(!ensureI2sReady() || !configureCodec()) {
        return false;
    }

    const uint32_t frameCount = (kSampleRate * durationMs) / 1000;
    if(frameCount == 0) {
        return false;
    }

    constexpr size_t kChunkFrames = 96;
    int16_t chunk[kChunkFrames * 2];
    float phase = 0.0f;
    const float twoPi = 6.28318530718f;

    for(uint32_t start = 0; start < frameCount; start += kChunkFrames) {
        const size_t count = static_cast<size_t>((frameCount - start) < kChunkFrames
                                                    ? (frameCount - start)
                                                    : kChunkFrames);
        for(size_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(start + i) / static_cast<float>(frameCount);
            const float hz = startHz + ((endHz - startHz) * t);
            const float step = (twoPi * hz) / static_cast<float>(kSampleRate);
            const float env = 0.25f + (0.75f * (1.0f - t));
            const int16_t sample = static_cast<int16_t>(sinf(phase) * (kAmplitude * env));
            chunk[(i * 2) + 0] = sample;
            chunk[(i * 2) + 1] = sample;
            phase += step;
            if(phase >= twoPi) {
                phase -= twoPi;
            }
        }
        const size_t bytes = count * 2 * sizeof(int16_t);
        const size_t written = g_audioI2s.write(reinterpret_cast<const uint8_t*>(chunk), bytes);
        if(written != bytes) {
            setDetail("I2S short write");
            return false;
        }
    }

    return true;
}

bool prepareAudio(bool forceRetry) {
    g_status.supported = isSupportedBoard();
    if(!g_status.supported) {
        setDetail("board has no audio helper");
        return false;
    }

    const bool ready = g_status.codecDetected && g_status.i2sReady && g_codecConfigured;
    if(ready && !forceRetry) {
        return true;
    }

    const uint32_t now = millis();
    if(!forceRetry && g_lastPrepareAttemptAt != 0 &&
       (now - g_lastPrepareAttemptAt) < kPrepareRetryMs) {
        return false;
    }

    g_lastPrepareAttemptAt = now;
    enableOutputStage();
    resetAudioTransportState();

    if(!probeCodec()) {
        setDetail("codec not found on i2c");
        return false;
    }
    g_status.codecDetected = true;

    if(!ensureI2sReady()) {
        return false;
    }

    if(!configureCodec()) {
        g_status.lastPlayOk = false;
        return false;
    }

    setDetail("codec ready");
    return true;
}

}  // namespace

void remote_audio_init() {
    if(!g_initialized) {
        g_initialized = true;
        g_status = {};
    }

    if(!prepareAudio(true)) {
        g_status.lastPlayOk = false;
    }
}

void remote_audio_service() {}

bool remote_audio_play(RemoteAudioTone tone) {
    if(!g_initialized) {
        remote_audio_init();
    }
    if(!g_status.supported) {
        g_status.lastPlayOk = false;
        ++g_status.failedPlays;
        return false;
    }
    if(!prepareAudio(false)) {
        g_status.lastPlayOk = false;
        ++g_status.failedPlays;
        return false;
    }

    bool ok = false;
    switch(tone) {
        case RemoteAudioTone::Click:
            ok = writeTone(1240.0f, 980.0f, 24);
            break;
        case RemoteAudioTone::SwipeLeft:
            ok = writeTone(1320.0f, 740.0f, 60);
            break;
        case RemoteAudioTone::SwipeRight:
            ok = writeTone(740.0f, 1320.0f, 60);
            break;
        case RemoteAudioTone::Sweep:
            ok = writeTone(520.0f, 1640.0f, 160);
            break;
    }

    g_status.lastPlayOk = ok;
    if(ok) {
        ++g_status.successfulPlays;
        setDetail("tone playback attempted");
    } else {
        ++g_status.failedPlays;
    }
    return ok;
}

RemoteAudioStatus remote_audio_status() {
    return g_status;
}

const char* remote_audio_detail() {
    return g_status.detail;
}
