#pragma once

#include <Arduino.h>
#include <sg98_remote_protocol.h>

void controller_env_poll();
String controller_env_build_frame();
bool controller_env_apply_peer_frame(const sg98_protocol::EnvironmentFrame& frame);
bool controller_env_time_valid();
bool controller_env_weather_valid();
uint32_t controller_env_last_ntp_sync_ms();
uint32_t controller_env_last_weather_fetch_ms();
String controller_env_location();
int16_t controller_env_temperature_f();
int8_t controller_env_precipitation_percent();
