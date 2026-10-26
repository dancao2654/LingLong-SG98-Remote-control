#include <cassert>
#include <cstdio>
#include "sg98_system_ota.h"

using namespace sg98_system_ota;

int main() {
    const auto none = planUpdates(true, true);
    assert(!none.anyNeeded());
    const auto controllerOnly = planUpdates(true, false);
    assert(!controllerOnly.remoteNeeded && controllerOnly.controllerNeeded);
    const auto remoteOnly = planUpdates(false, true);
    assert(remoteOnly.remoteNeeded && !remoteOnly.controllerNeeded);
    const auto both = planUpdates(false, false);
    assert(both.remoteNeeded && both.controllerNeeded);
    Flow flow;
    assert(!flow.busy());
    flow.start(100);
    assert(flow.stage == Stage::Connect && flow.cancellable());
    assert(!flow.receive(Event::FilesReady, 101));
    assert(flow.receive(Event::ControllerReady, 102));
    assert(flow.stage == Stage::WifiConnect && flow.cancellable());
    assert(flow.receive(Event::WifiConnected, 103));
    assert(flow.stage == Stage::DownloadFiles && !flow.cancellable());
    assert(!flow.expired(600102));
    assert(flow.expired(600103));
    assert(flow.receive(Event::FilesReady, 104));
    assert(flow.receive(Event::LocalConnected, 105));
    assert(flow.stage == Stage::ControllerTransfer);
    assert(flow.receive(Event::TransferReady, 106));
    assert(flow.receive(Event::ControllerVerified, 107));
    assert(flow.controllerVerified && flow.stage == Stage::RemoteInstall);
    assert(flow.receive(Event::Failure, 108));
    assert(flow.stage == Stage::Failed && flow.failedStage == Stage::RemoteInstall);

    flow.start(200);
    assert(flow.receive(Event::ControllerReady, 201));
    assert(flow.receive(Event::WifiConnected, 202));
    assert(flow.receive(Event::FilesReady, 203));
    assert(flow.receive(Event::LegacyUpdateRequired, 204));
    assert(flow.stage == Stage::LegacyControllerUpdate);
    assert(!flow.expired(600203));
    assert(flow.expired(600204));

    flow.start(UINT32_MAX - 1000);
    assert(!flow.expired(28000));
    assert(flow.expired(29000));
    std::puts("PASS: coordinated local-push OTA plans, sequencing and timeouts");
}
