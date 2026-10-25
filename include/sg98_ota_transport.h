#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace sg98_ota_transport {

constexpr uint32_t kMagic = 0x544F4753UL;  // "SGOT" on the wire.
constexpr uint8_t kVersion = 1;
constexpr size_t kDataBytes = 220;
constexpr size_t kSha256Bytes = 32;
constexpr size_t kVersionBytes = 24;
constexpr size_t kMessageBytes = 48;

enum class Type : uint8_t { Begin = 1, Data = 2, End = 3, Commit = 4, Abort = 5, Ack = 6 };
enum class Status : uint8_t { Accepted = 0, Ready = 1, Committed = 2, Busy = 3, Error = 4 };

#pragma pack(push, 1)
struct Header {
    uint32_t magic = kMagic;
    uint8_t version = kVersion;
    Type type = Type::Abort;
    uint16_t session = 0;
};

struct BeginFrame {
    Header header;
    uint32_t imageSize = 0;
    uint8_t sha256[kSha256Bytes] = {};
    char release[kVersionBytes] = {};
};

struct DataFrame {
    Header header;
    uint32_t offset = 0;
    uint16_t length = 0;
    uint32_t crc32 = 0;
    uint8_t data[kDataBytes] = {};
};

struct ControlFrame {
    Header header;
};

struct AckFrame {
    Header header;
    uint32_t nextOffset = 0;
    Status status = Status::Error;
    uint8_t progressPercent = 0;
    char message[kMessageBytes] = {};
};
#pragma pack(pop)

static_assert(sizeof(DataFrame) <= 250, "ESP-NOW OTA frame exceeds payload limit");

inline bool isFrame(const uint8_t* data, size_t length) {
    if(data == nullptr || length < sizeof(Header)) return false;
    Header header = {};
    memcpy(&header, data, sizeof(header));
    return header.magic == kMagic && header.version == kVersion;
}

inline uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for(size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

inline void setMessage(AckFrame* ack, const char* message) {
    if(ack == nullptr) return;
    const char* source = message != nullptr ? message : "";
    strncpy(ack->message, source, sizeof(ack->message) - 1);
    ack->message[sizeof(ack->message) - 1] = '\0';
}

}  // namespace sg98_ota_transport
