#pragma once

#include <stdint.h>

namespace sg98_connection_reveal {

struct State {
    bool online = false;
    bool armed = true;
    bool offlineTiming = false;
    bool active = false;
    uint32_t offlineSince = 0;
    uint32_t startedAt = 0;
    uint32_t count = 0;

    bool observe(bool freshStatus, bool eligible, uint32_t now) {
        if(!freshStatus) {
            online = false;
            if(!offlineTiming) { offlineTiming = true; offlineSince = now; }
            if(static_cast<uint32_t>(now - offlineSince) >= 5000) armed = true;
            active = false;
            return false;
        }
        offlineTiming = false;
        if(online) return false;
        online = true;
        const bool show = armed && eligible;
        armed = false;
        if(show) { active = true; startedAt = now; ++count; }
        return show;
    }

    void service(bool eligible, bool input, uint32_t now) {
        if(!eligible || input || static_cast<uint32_t>(now - startedAt) >= 1500) active = false;
    }
};

}  // namespace sg98_connection_reveal
