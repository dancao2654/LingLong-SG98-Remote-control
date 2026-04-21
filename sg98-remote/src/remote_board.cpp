#include "remote_board_driver.h"

namespace {

class NullCanvas : public RemoteCanvas {
  public:
    uint16_t width() const override {
        return 240;
    }

    uint16_t height() const override {
        return 240;
    }

    void fillScreen(uint16_t) override {}
    void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) override {}
    void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) override {}
    void fillRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, uint16_t) override {}
    void drawRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, uint16_t) override {}
    void fillCircle(int16_t, int16_t, int16_t, uint16_t) override {}
    void drawCircle(int16_t, int16_t, int16_t, uint16_t) override {}
    void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) override {}
    void drawCenteredText(const String&, int16_t, int16_t, RemoteTextStyle, uint16_t, uint16_t) override {}
    void drawTextTopLeft(const String&, int16_t, int16_t, RemoteTextStyle, uint16_t, uint16_t) override {}
    void drawTextTopRight(const String&, int16_t, int16_t, RemoteTextStyle, uint16_t, uint16_t) override {}
};

NullCanvas g_nullCanvas;

const RemoteBoardInfo kFallbackBoardInfo = {
    RemoteBoardId::CrowPanelRotary,
    "Unknown Remote Board",
    240,
    240,
    false,
    false,
    0,
};

const RemoteBoardServices kFallbackBoardServices = {};
const RemoteBoardPins kFallbackBoardPins = {};
const RemoteBoardFeatureMap kFallbackBoardFeatureMap = {};
const RemoteBatteryStatus kFallbackBatteryStatus = {};

const RemoteBoardDriver* g_activeDriver = nullptr;

const RemoteBoardDriver* const kDrivers[] = {
    remote_board_crowpanel_driver(),
    remote_board_crowpanel_rotary21_driver(),
    remote_board_waveshare_amoled175_driver(),
};

}  // namespace

bool remote_board_init() {
    g_activeDriver = nullptr;

    for(const RemoteBoardDriver* driver : kDrivers) {
        if(driver == nullptr || driver->probe == nullptr || driver->init == nullptr) {
            continue;
        }

        remote_board_console().printf("[remote] probing %s\n", driver->key);
        if(!driver->probe()) {
            continue;
        }

        remote_board_console().printf("[remote] detected %s\n", driver->key);
        if(driver->init()) {
            g_activeDriver = driver;
            remote_board_console().printf("[remote] active board: %s\n", driver->info().name);
            return true;
        }

        remote_board_console().printf("[remote] init failed for %s\n", driver->key);
    }

    remote_board_console().println("[remote] no supported board detected");
    return false;
}

void remote_board_poll_input(RemoteInputState* input) {
    if(input == nullptr) {
        return;
    }

    *input = {};
    if(g_activeDriver != nullptr && g_activeDriver->poll_input != nullptr) {
        g_activeDriver->poll_input(input);
    }
}

RemoteCanvas& remote_board_canvas() {
    if(g_activeDriver != nullptr && g_activeDriver->canvas != nullptr) {
        return g_activeDriver->canvas();
    }
    return g_nullCanvas;
}

const RemoteBoardInfo& remote_board_info() {
    if(g_activeDriver != nullptr && g_activeDriver->info != nullptr) {
        return g_activeDriver->info();
    }
    return kFallbackBoardInfo;
}

const RemoteBoardServices& remote_board_services() {
    if(g_activeDriver != nullptr && g_activeDriver->services != nullptr) {
        return g_activeDriver->services();
    }
    return kFallbackBoardServices;
}

const RemoteBoardPins& remote_board_pins() {
    if(g_activeDriver != nullptr && g_activeDriver->pins != nullptr) {
        return g_activeDriver->pins();
    }
    return kFallbackBoardPins;
}

const RemoteBoardFeatureMap& remote_board_feature_map() {
    if(g_activeDriver != nullptr && g_activeDriver->feature_map != nullptr) {
        return g_activeDriver->feature_map();
    }
    return kFallbackBoardFeatureMap;
}

Print& remote_board_console() {
    if(g_activeDriver != nullptr && g_activeDriver->services != nullptr) {
        const RemoteBoardServices& services = g_activeDriver->services();
        if(services.console != nullptr) {
            return *services.console;
        }
    }
    return Serial;
}

void remote_board_apply_indicator(const RemoteIndicatorState& indicator) {
    if(g_activeDriver != nullptr && g_activeDriver->apply_indicator != nullptr) {
        g_activeDriver->apply_indicator(indicator);
    }
}

bool remote_board_set_backlight(uint8_t level) {
    if(g_activeDriver != nullptr && g_activeDriver->set_backlight != nullptr) {
        return g_activeDriver->set_backlight(level);
    }
    return false;
}

RemoteBatteryStatus remote_board_battery_status() {
    if(g_activeDriver != nullptr) {
        if(g_activeDriver->battery_status != nullptr) {
            return g_activeDriver->battery_status();
        }
        if(g_activeDriver->battery_millivolts != nullptr) {
            RemoteBatteryStatus status = {};
            status.millivolts = g_activeDriver->battery_millivolts();
            status.supported = status.millivolts >= 0;
            status.batteryPresent = status.supported;
            return status;
        }
    }
    return kFallbackBatteryStatus;
}

int32_t remote_board_battery_millivolts() {
    return remote_board_battery_status().millivolts;
}
