#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sg98_ota_transport.h"

bool controller_ota_transport_handle(const uint8_t* data,
                                     size_t length,
                                     sg98_ota_transport::AckFrame* ack);
void controller_ota_transport_service();
bool controller_ota_transport_active();
