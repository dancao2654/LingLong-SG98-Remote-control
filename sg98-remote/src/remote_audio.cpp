#include "remote_audio.h"

#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
std::atomic<bool> g_revealBusy{false};
std::atomic<bool> g_revealCancel{false};
std::atomic<bool> g_revealFinished{false};
std::atomic<bool> g_revealOk{false};

void revealAudioWorker(void*) {
    constexpr float twoPi = 6.28318530718f;
    constexpr uint32_t frames = kSampleRate * 1350 / 1000;
    constexpr size_t chunkFrames = 96;
    int16_t chunk[chunkFrames * 2];
    float phase = 0.0f;
    bool ok = true;
    // A rising, airy reveal settles into a soft C-major voicing, then fades away.
    for(uint32_t offset = 0; offset < frames && !g_revealCancel.load(); offset += chunkFrames) {
        const size_t count = min<size_t>(chunkFrames, frames - offset);
        for(size_t i = 0; i < count; ++i) {
            const float seconds = static_cast<float>(offset + i) / kSampleRate;
            const float rise = min(1.0f, seconds / 0.52f);
            const float attack = min(1.0f, seconds / 0.12f);
            const float release = min(1.0f, (1.35f - seconds) / 0.65f);
            const float hz = 330.0f + 330.0f * rise * rise;
            phase += twoPi * hz / kSampleRate;
            if(phase > twoPi) phase -= twoPi;
            const float shimmer = (sinf(phase) + 0.25f * sinf(phase * 3.0f)) * (1.0f - rise);
            const float chord = (sinf(twoPi * 523.25f * seconds) +
                                 0.8f * sinf(twoPi * 659.25f * seconds) +
                                 0.65f * sinf(twoPi * 783.99f * seconds)) / 2.45f;
            const int16_t sample = static_cast<int16_t>((shimmer + chord * rise) *
                                                        kAmplitude * 0.65f * attack * release);
            chunk[i * 2] = sample;
            chunk[i * 2 + 1] = sample;
        }
        const size_t bytes = count * 2 * sizeof(int16_t);
        if(g_audioI2s.write(reinterpret_cast<const uint8_t*>(chunk), bytes) != bytes) {
            ok = false;
            break;
        }
    }
    g_revealOk.store(ok);
    g_revealBusy.store(false);
    g_revealFinished.store(true);
    vTaskDelete(nullptr);
}

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

void writePcmWavHeader(uint8_t* out, uint32_t dataSize, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels) {
    if(out == nullptr) {
        return;
    }

    const uint32_t riffSize = dataSize + 36;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = channels * (bitsPerSample / 8);

    memcpy(out + 0, "RIFF", 4);
    out[4] = static_cast<uint8_t>(riffSize & 0xFF);
    out[5] = static_cast<uint8_t>((riffSize >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>((riffSize >> 16) & 0xFF);
    out[7] = static_cast<uint8_t>((riffSize >> 24) & 0xFF);
    memcpy(out + 8, "WAVEfmt ", 8);
    out[16] = 16;
    out[17] = 0;
    out[18] = 0;
    out[19] = 0;
    out[20] = 1;
    out[21] = 0;
    out[22] = static_cast<uint8_t>(channels & 0xFF);
    out[23] = static_cast<uint8_t>((channels >> 8) & 0xFF);
    out[24] = static_cast<uint8_t>(sampleRate & 0xFF);
    out[25] = static_cast<uint8_t>((sampleRate >> 8) & 0xFF);
    out[26] = static_cast<uint8_t>((sampleRate >> 16) & 0xFF);
    out[27] = static_cast<uint8_t>((sampleRate >> 24) & 0xFF);
    out[28] = static_cast<uint8_t>(byteRate & 0xFF);
    out[29] = static_cast<uint8_t>((byteRate >> 8) & 0xFF);
    out[30] = static_cast<uint8_t>((byteRate >> 16) & 0xFF);
    out[31] = static_cast<uint8_t>((byteRate >> 24) & 0xFF);
    out[32] = static_cast<uint8_t>(blockAlign & 0xFF);
    out[33] = static_cast<uint8_t>((blockAlign >> 8) & 0xFF);
    out[34] = static_cast<uint8_t>(bitsPerSample & 0xFF);
    out[35] = static_cast<uint8_t>((bitsPerSample >> 8) & 0xFF);
    memcpy(out + 36, "data", 4);
    out[40] = static_cast<uint8_t>(dataSize & 0xFF);
    out[41] = static_cast<uint8_t>((dataSize >> 8) & 0xFF);
    out[42] = static_cast<uint8_t>((dataSize >> 16) & 0xFF);
    out[43] = static_cast<uint8_t>((dataSize >> 24) & 0xFF);
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
    if(g_revealBusy.load()) return;
    if(!g_initialized) {
        g_initialized = true;
        g_status = {};
    }

    if(!prepareAudio(true)) {
        g_status.lastPlayOk = false;
    }
}

void remote_audio_service() {
    if(g_revealFinished.exchange(false)) {
        g_status.lastPlayOk = g_revealOk.load();
        if(g_status.lastPlayOk) ++g_status.successfulPlays;
        else ++g_status.failedPlays;
        setDetail(g_status.lastPlayOk ? "reveal playback complete" : "reveal playback failed");
    }
}

void remote_audio_cancel_reveal() { g_revealCancel.store(true); }

bool remote_audio_play(RemoteAudioTone tone) {
    if(g_revealBusy.load()) return false;
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
    if(tone == RemoteAudioTone::Reveal) {
        g_revealCancel.store(false);
        g_revealBusy.store(true);
        if(xTaskCreate(revealAudioWorker, "sg98-reveal", 3072, nullptr, 1, nullptr) != pdPASS) {
            g_revealBusy.store(false);
            g_status.lastPlayOk = false;
            ++g_status.failedPlays;
            setDetail("reveal audio task unavailable");
            return false;
        }
        return true;
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
        case RemoteAudioTone::Reveal:
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

bool remote_audio_record_wav(uint32_t durationMs, uint8_t** outBuffer, size_t* outSize) {
    if(g_revealBusy.load()) return false;
    if(outBuffer != nullptr) {
        *outBuffer = nullptr;
    }
    if(outSize != nullptr) {
        *outSize = 0;
    }
    if(outBuffer == nullptr || outSize == nullptr) {
        setDetail("record output missing");
        return false;
    }
    if(!g_initialized) {
        remote_audio_init();
    }
    if(!g_status.supported) {
        setDetail("board has no audio input");
        return false;
    }
    if(!prepareAudio(false)) {
        return false;
    }

    if(!g_audioI2s.configureRX(kSampleRate,
                               I2S_DATA_BIT_WIDTH_16BIT,
                               I2S_SLOT_MODE_STEREO,
                               I2S_RX_TRANSFORM_16_STEREO_TO_MONO)) {
        setDetail("I2S RX config failed");
        return false;
    }

    constexpr size_t kWavHeaderSize = 44;
    const uint32_t boundedDurationMs = durationMs < 500 ? 500 : (durationMs > 5000 ? 5000 : durationMs);
    const uint32_t pcmBytes = ((kSampleRate * boundedDurationMs) / 1000UL) * sizeof(int16_t);
    const size_t totalBytes = kWavHeaderSize + pcmBytes;
    uint8_t* wav = static_cast<uint8_t*>(malloc(totalBytes));
    if(wav == nullptr) {
        setDetail("record malloc failed");
        return false;
    }

    writePcmWavHeader(wav, pcmBytes, kSampleRate, 16, 1);
    const size_t recorded = g_audioI2s.readBytes(reinterpret_cast<char*>(wav + kWavHeaderSize), pcmBytes);
    if(recorded != pcmBytes) {
        free(wav);
        setDetail("I2S short record");
        return false;
    }

    *outBuffer = wav;
    *outSize = totalBytes;
    setDetail("recorded wav");
    return true;
}

void remote_audio_free_recording(uint8_t* buffer) {
    free(buffer);
}

RemoteAudioStatus remote_audio_status() {
    return g_status;
}

const char* remote_audio_detail() {
    return g_status.detail;
}
