#include "remote_runtime.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

TaskHandle_t g_loopTask = nullptr;

}  // namespace

void remote_runtime_bind_current_task() {
    g_loopTask = xTaskGetCurrentTaskHandle();
}

void remote_runtime_notify_input() {
    if(g_loopTask != nullptr) {
        xTaskNotifyGive(g_loopTask);
    }
}

void IRAM_ATTR remote_runtime_notify_input_from_isr() {
    if(g_loopTask == nullptr) {
        return;
    }

    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(g_loopTask, &higherPriorityTaskWoken);
    if(higherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

bool remote_runtime_wait_for_input_or_timeout(uint32_t timeoutMs) {
    const TickType_t ticks = timeoutMs == 0 ? 0 : pdMS_TO_TICKS(timeoutMs);
    return ulTaskNotifyTake(pdTRUE, ticks) > 0;
}
