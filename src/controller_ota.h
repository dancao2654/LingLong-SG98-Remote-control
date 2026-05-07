#pragma once

#include <Arduino.h>

#include "sg98_ota_download.h"

void controller_ota_init();
bool controller_ota_note_downloaded(const sg98_ota_download::Result& result);
bool controller_ota_ready_for_remote();
String controller_ota_ready_detail();
void controller_ota_clear_ready();
bool controller_ota_mark_ready_bootable(String* message);
bool controller_ota_invalidate_inactive_slot(String* message);
