#pragma once

#include <Arduino.h>
#include <sg98_remote_protocol.h>

void controller_env_poll();
String controller_env_build_frame();
bool controller_env_time_valid();
bool controller_env_weather_valid();
