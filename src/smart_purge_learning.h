#pragma once

#include <Arduino.h>
#include <stddef.h>
void smart_purge_learning_init();
void smart_purge_learning_reset_runtime();
void smart_purge_learning_note_sample(int16_t targetRpm,
                                      int16_t currentRpm,
                                      int16_t torque,
                                      uint16_t baselineTorque,
                                      uint16_t manualThreshold);
uint16_t smart_purge_learning_threshold(int16_t targetRpm,
                                        uint16_t baselineTorque,
                                        uint16_t manualThreshold);
void smart_purge_learning_service_persist(bool idleState, unsigned long lastActivityAt);

size_t smart_purge_learning_bucket_count();
