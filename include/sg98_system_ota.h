#pragma once

#include <stdint.h>

namespace sg98_system_ota {

struct UpdatePlan {
    bool remoteNeeded;
    bool controllerNeeded;
    bool anyNeeded() const { return remoteNeeded || controllerNeeded; }
};

inline UpdatePlan planUpdates(bool remoteCurrent, bool controllerCurrent) {
    return {!remoteCurrent, !controllerCurrent};
}

enum class Stage : uint8_t {
    Idle, Connect, WifiConnect, DownloadFiles, LocalConnect,
    LegacyControllerUpdate, ControllerTransfer, ControllerVerify,
    RemoteInstall, Complete, Failed
};

enum class Event : uint8_t {
    ControllerReady, WifiConnected, FilesReady, LocalConnected,
    LegacyUpdateRequired, TransferReady, ControllerVerified, Failure
};

struct Flow {
    Stage stage = Stage::Idle;
    Stage failedStage = Stage::Idle;
    uint32_t enteredAt = 0;
    bool controllerVerified = false;

    bool busy() const { return stage >= Stage::Connect && stage <= Stage::RemoteInstall; }
    bool cancellable() const {
        return busy() && stage != Stage::DownloadFiles &&
               stage != Stage::LegacyControllerUpdate &&
               stage != Stage::ControllerTransfer && stage != Stage::ControllerVerify &&
               stage != Stage::RemoteInstall;
    }
    void enter(Stage next, uint32_t now) { stage = next; enteredAt = now; }
    void start(uint32_t now) {
        controllerVerified = false;
        failedStage = Stage::Idle;
        enter(Stage::Connect, now);
    }
    void fail(uint32_t now) {
        failedStage = stage;
        enter(Stage::Failed, now);
    }
    bool expired(uint32_t now) const {
        uint32_t limit = 30000;
        if(stage == Stage::WifiConnect) limit = 45000;
        if(stage == Stage::DownloadFiles) limit = 600000;
        if(stage == Stage::LocalConnect) limit = 60000;
        if(stage == Stage::LegacyControllerUpdate) limit = 600000;
        if(stage == Stage::ControllerTransfer) limit = 600000;
        if(stage == Stage::ControllerVerify) limit = 60000;
        return busy() && static_cast<uint32_t>(now - enteredAt) >= limit;
    }
    bool receive(Event event, uint32_t now) {
        if(!busy()) return false;
        if(event == Event::Failure) {
            fail(now);
            return true;
        }
        Stage next = stage;
        switch(stage) {
            case Stage::Connect:
                if(event == Event::ControllerReady) next = Stage::WifiConnect;
                break;
            case Stage::WifiConnect:
                if(event == Event::WifiConnected) next = Stage::DownloadFiles;
                break;
            case Stage::DownloadFiles:
                if(event == Event::FilesReady) next = Stage::LocalConnect;
                break;
            case Stage::LocalConnect:
                if(event == Event::LocalConnected) next = Stage::ControllerTransfer;
                if(event == Event::LegacyUpdateRequired) next = Stage::LegacyControllerUpdate;
                break;
            case Stage::ControllerTransfer:
                if(event == Event::TransferReady) next = Stage::ControllerVerify;
                break;
            case Stage::ControllerVerify:
                if(event == Event::ControllerVerified) {
                    controllerVerified = true;
                    next = Stage::RemoteInstall;
                }
                break;
            default: break;
        }
        if(next == stage) return false;
        enter(next, now);
        return true;
    }
};

inline const char* label(Stage stage) {
    switch(stage) {
        case Stage::Idle: return "Ready";
        case Stage::Connect: return "Checking controller";
        case Stage::WifiConnect: return "Connecting home Wi-Fi";
        case Stage::DownloadFiles: return "Downloading update files";
        case Stage::LocalConnect: return "Reconnecting controller";
        case Stage::LegacyControllerUpdate: return "Updating legacy controller";
        case Stage::ControllerTransfer: return "Sending controller firmware";
        case Stage::ControllerVerify: return "Verifying controller";
        case Stage::RemoteInstall: return "Installing remote firmware";
        case Stage::Complete: return "Both devices current";
        case Stage::Failed: return "Update stopped";
    }
    return "Update stopped";
}

}  // namespace sg98_system_ota
