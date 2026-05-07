#pragma once

#include "remote_board.h"

struct RemoteBoardDriver {
    const char* key;
    bool (*probe)();
    bool (*init)();
    void (*poll_input)(RemoteInputState* input);
    RemoteCanvas& (*canvas)();
    const RemoteBoardInfo& (*info)();
    const RemoteBoardServices& (*services)();
    const RemoteBoardPins& (*pins)();
    const RemoteBoardFeatureMap& (*feature_map)();
    void (*apply_indicator)(const RemoteIndicatorState& indicator);
    bool (*set_backlight)(uint8_t level);
    RemoteBatteryStatus (*battery_status)();
    int32_t (*battery_millivolts)();
    bool (*input_pending)();
};

const RemoteBoardDriver* remote_board_crowpanel_driver();
const RemoteBoardDriver* remote_board_crowpanel_rotary21_driver();
const RemoteBoardDriver* remote_board_waveshare_amoled175_driver();
const RemoteBoardDriver* remote_board_lilygo_t4_s3_h678_driver();
