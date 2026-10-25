#include <cassert>
#include <cstdio>
#include <cstring>

#include "sg98_ota_transport.h"

using namespace sg98_ota_transport;

int main() {
    static_assert(sizeof(DataFrame) <= 250, "wire payload must fit ESP-NOW");
    DataFrame frame;
    frame.header.type = Type::Data;
    frame.header.session = 42;
    frame.offset = 440;
    frame.length = 4;
    memcpy(frame.data, "SG98", frame.length);
    frame.crc32 = crc32(frame.data, frame.length);
    assert(isFrame(reinterpret_cast<const uint8_t*>(&frame), sizeof(Header)));
    assert(frame.crc32 == 0xABAFD131UL);
    frame.data[0] ^= 1;
    assert(frame.crc32 != crc32(frame.data, frame.length));

    AckFrame ack;
    ack.header.type = Type::Ack;
    ack.header.session = frame.header.session;
    ack.nextOffset = frame.offset + frame.length;
    ack.status = Status::Accepted;
    setMessage(&ack, "chunk");
    assert(ack.nextOffset == 444);
    assert(strcmp(ack.message, "chunk") == 0);
    std::puts("PASS: OTA wire size, identification, CRC, and acknowledgement");
}
