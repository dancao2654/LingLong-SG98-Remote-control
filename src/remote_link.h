#pragma once

#include <Arduino.h>

String remote_link_build_status_frame();
String remote_link_build_menu_frame(uint8_t menuNum);
bool remote_link_handle_command(const String& rawPayload, String* message, bool* sendStatus, int8_t* menuRefresh);
