#pragma once

#include <Arduino.h>
#include <stdint.h>

void remote_runtime_bind_current_task();
void remote_runtime_notify_input();
void IRAM_ATTR remote_runtime_notify_input_from_isr();
bool remote_runtime_wait_for_input_or_timeout(uint32_t timeoutMs);
