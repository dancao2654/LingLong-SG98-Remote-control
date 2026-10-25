#include "controller_ota_transport.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "controller_ota.h"
#include "main.h"
#include "sg98_ota_download.h"

namespace {

struct TransferState {
    bool active = false;
    bool shaActive = false;
    uint16_t session = 0;
    uint32_t imageSize = 0;
    uint32_t received = 0;
    uint8_t expectedSha[sg98_ota_transport::kSha256Bytes] = {};
    char release[sg98_ota_transport::kVersionBytes] = {};
    esp_ota_handle_t otaHandle = 0;
    const esp_partition_t* partition = nullptr;
    mbedtls_sha256_context sha;
};

TransferState g_transfer;
uint32_t g_restartAt = 0;

void resetTransfer(bool abortOta) {
    if(abortOta && g_transfer.active && g_transfer.otaHandle != 0) {
        (void)esp_ota_abort(g_transfer.otaHandle);
    }
    if(g_transfer.shaActive) {
        mbedtls_sha256_free(&g_transfer.sha);
    }
    g_transfer = {};
}

void prepareAck(sg98_ota_transport::AckFrame* ack,
                const sg98_ota_transport::Header& request,
                sg98_ota_transport::Status status,
                const char* message) {
    if(ack == nullptr) return;
    *ack = {};
    ack->header.type = sg98_ota_transport::Type::Ack;
    ack->header.session = request.session;
    ack->status = status;
    ack->nextOffset = g_transfer.received;
    ack->progressPercent = g_transfer.imageSize == 0
                               ? 0
                               : static_cast<uint8_t>((g_transfer.received * 100ULL) / g_transfer.imageSize);
    sg98_ota_transport::setMessage(ack, message);
}

bool beginTransfer(const sg98_ota_transport::BeginFrame& frame,
                   sg98_ota_transport::AckFrame* ack) {
    if(state != IDLE) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Busy, "controller not idle");
        return true;
    }
    if(g_transfer.active && g_transfer.session == frame.header.session &&
       g_transfer.imageSize == frame.imageSize) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Accepted, "resume");
        return true;
    }
    resetTransfer(true);
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if(partition == nullptr || frame.imageSize == 0 || frame.imageSize > partition->size) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "invalid image size");
        return true;
    }
    esp_ota_handle_t handle = 0;
    const esp_err_t beginError = esp_ota_begin(partition, frame.imageSize, &handle);
    if(beginError != ESP_OK) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "ota begin failed");
        return true;
    }
    g_transfer.active = true;
    g_transfer.session = frame.header.session;
    g_transfer.imageSize = frame.imageSize;
    g_transfer.otaHandle = handle;
    g_transfer.partition = partition;
    memcpy(g_transfer.expectedSha, frame.sha256, sizeof(g_transfer.expectedSha));
    memcpy(g_transfer.release, frame.release, sizeof(g_transfer.release));
    g_transfer.release[sizeof(g_transfer.release) - 1] = '\0';
    mbedtls_sha256_init(&g_transfer.sha);
    g_transfer.shaActive = mbedtls_sha256_starts(&g_transfer.sha, 0) == 0;
    if(!g_transfer.shaActive) {
        resetTransfer(true);
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "sha init failed");
        return true;
    }
    controller_ota_clear_ready();
    prepareAck(ack, frame.header, sg98_ota_transport::Status::Accepted, "ready");
    return true;
}

bool receiveData(const sg98_ota_transport::DataFrame& frame,
                 size_t length,
                 sg98_ota_transport::AckFrame* ack) {
    if(!g_transfer.active || frame.header.session != g_transfer.session) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "no transfer");
        return true;
    }
    const size_t required = offsetof(sg98_ota_transport::DataFrame, data) + frame.length;
    if(frame.length == 0 || frame.length > sizeof(frame.data) || length < required ||
       frame.offset + frame.length > g_transfer.imageSize) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "bad chunk");
        return true;
    }
    if(frame.offset < g_transfer.received) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Accepted, "duplicate");
        return true;
    }
    if(frame.offset != g_transfer.received ||
       sg98_ota_transport::crc32(frame.data, frame.length) != frame.crc32) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "chunk mismatch");
        return true;
    }
    if(esp_ota_write(g_transfer.otaHandle, frame.data, frame.length) != ESP_OK ||
       mbedtls_sha256_update(&g_transfer.sha, frame.data, frame.length) != 0) {
        resetTransfer(true);
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "flash write failed");
        return true;
    }
    g_transfer.received += frame.length;
    prepareAck(ack, frame.header, sg98_ota_transport::Status::Accepted, "chunk");
    return true;
}

bool finishTransfer(const sg98_ota_transport::ControlFrame& frame,
                    sg98_ota_transport::AckFrame* ack) {
    if(!g_transfer.active || frame.header.session != g_transfer.session ||
       g_transfer.received != g_transfer.imageSize) {
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "image incomplete");
        return true;
    }
    uint8_t digest[sg98_ota_transport::kSha256Bytes] = {};
    if(mbedtls_sha256_finish(&g_transfer.sha, digest) != 0) {
        resetTransfer(true);
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "sha finish failed");
        return true;
    }
    mbedtls_sha256_free(&g_transfer.sha);
    g_transfer.shaActive = false;
    if(memcmp(digest, g_transfer.expectedSha, sizeof(digest)) != 0) {
        resetTransfer(true);
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "image sha mismatch");
        return true;
    }
    if(esp_ota_end(g_transfer.otaHandle) != ESP_OK) {
        g_transfer.otaHandle = 0;
        resetTransfer(false);
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "image validation failed");
        return true;
    }
    g_transfer.otaHandle = 0;
    sg98_ota_download::Result result;
    result.ok = true;
    result.verified = true;
    result.written = g_transfer.received;
    result.expected = g_transfer.imageSize;
    result.version = String(g_transfer.release);
    result.sha256 = sg98_ota_download::sha256ToHex(digest);
    if(!controller_ota_note_downloaded(result)) {
        resetTransfer(false);
        prepareAck(ack, frame.header, sg98_ota_transport::Status::Error, "ready marker failed");
        return true;
    }
    prepareAck(ack, frame.header, sg98_ota_transport::Status::Ready, "verified");
    g_transfer.active = false;
    return true;
}

}  // namespace

bool controller_ota_transport_handle(const uint8_t* data,
                                     size_t length,
                                     sg98_ota_transport::AckFrame* ack) {
    using namespace sg98_ota_transport;
    if(!isFrame(data, length) || ack == nullptr) return false;
    Header header = {};
    memcpy(&header, data, sizeof(header));
    if(header.type == Type::Begin && length >= sizeof(BeginFrame)) {
        BeginFrame frame = {};
        memcpy(&frame, data, sizeof(frame));
        return beginTransfer(frame, ack);
    }
    if(header.type == Type::Data && length >= offsetof(DataFrame, data)) {
        DataFrame frame = {};
        memcpy(&frame, data, min(length, sizeof(frame)));
        return receiveData(frame, length, ack);
    }
    if(header.type == Type::End && length >= sizeof(ControlFrame)) {
        ControlFrame frame = {};
        memcpy(&frame, data, sizeof(frame));
        return finishTransfer(frame, ack);
    }
    if(header.type == Type::Commit && length >= sizeof(ControlFrame)) {
        String message;
        const bool ready = controller_ota_mark_ready_bootable(&message);
        prepareAck(ack, header, ready ? Status::Committed : Status::Error,
                   ready ? "rebooting" : message.c_str());
        if(ready) g_restartAt = millis() + 500;
        return true;
    }
    if(header.type == Type::Abort) {
        resetTransfer(true);
        prepareAck(ack, header, Status::Accepted, "aborted");
        return true;
    }
    prepareAck(ack, header, Status::Error, "unsupported frame");
    return true;
}

void controller_ota_transport_service() {
    if(g_restartAt != 0 && static_cast<int32_t>(millis() - g_restartAt) >= 0) {
        Serial.println("[ota_push] commit acknowledged; rebooting");
        Serial.flush();
        ESP.restart();
    }
}

bool controller_ota_transport_active() {
    return g_transfer.active;
}
