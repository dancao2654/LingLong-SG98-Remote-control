#include "kfm_zero_probe.h"
#include <limits>

namespace kfm_zero_probe {
const char* phaseName(Phase p) {
    switch(p) {
        case Phase::Idle: return "idle";
        case Phase::Preflight: return "preflight";
        case Phase::VerifyHold: return "verify_zero_hold";
        case Phase::WaitingForGap: return "waiting_for_power_off";
        case Phase::AwaitReturn: return "waiting_for_power_on";
        case Phase::VerifyReturn: return "verify_post_gap_state";
        case Phase::Complete: return "complete";
        case Phase::Cancelled: return "cancelled";
        case Phase::Failed: return "failed";
    }
    return "unknown";
}
bool Probe::active() const {
    return view_.phase == Phase::Preflight || view_.phase == Phase::VerifyHold ||
           view_.phase == Phase::WaitingForGap || view_.phase == Phase::AwaitReturn ||
           view_.phase == Phase::VerifyReturn;
}
bool Probe::start(uint32_t now) {
    if(active()) return false;
    view_ = {}; view_.phase = Phase::Preflight; view_.reason = "checking_zero_preset";
    view_.startedAt = now; polled_ = false;
    return true;
}
bool Probe::sample(const Io& io, Sample& out) {
    if(!io.read) return false;
    Sample s = {}; uint16_t rpm = 0;
    if(!io.read(22, 4, s.source) || !io.read(0x010F, 5, s.forced) ||
       !io.read(0x0100, 4, s.inputs) || !io.read(0x011E, 2, s.virtualInputs) ||
       !io.read(4, 1, &s.mode) || !io.read(53, 1, &s.softwareEnable) || !io.read(0x1000, 1, &rpm) ||
       !io.read(0x1013, 1, &s.alarm) || !io.read(0x1012, 1, &s.busVoltageRaw) ||
       !io.read(0x1016, 1, &s.operatingStatusRaw) || !io.read(0x100F, 1, &s.inputsStatusRaw)) return false;
    s.rpm = static_cast<int16_t>(rpm); out = s; return true;
}
const char* Probe::invalid(const Sample& s, uint16_t son) const {
    if(s.source[0] != 1 || s.source[3] != 0 || s.mode != 1) return "zero_preset_or_mode_mismatch";
    if(s.softwareEnable != 0) return "software_enable_not_zero";
    if(s.forced[0] != son || s.forced[1] != 2 || s.forced[2] != 0 ||
       s.forced[3] != 0 || s.forced[4] != 0) return "forced_input_mismatch";
    for(uint16_t i=0;i<4;++i) if(s.inputs[i] != i+1) return "factory_input_mapping_mismatch";
    if(s.virtualInputs[0] || s.virtualInputs[1]) return "virtual_input_mismatch";
    if(s.rpm != 0) return "unexpected_rotation";
    if(s.alarm != 0) return "drive_alarm";
    return nullptr;
}
bool Probe::restore(const Io& io) {
    if(!view_.enableAttempted) return true;
    view_.restoreAttempted = true;
    // All stop commands must be attempted even when an earlier ACK is lost.
    const bool sonOff = io.write && io.write(0x010F, 0);
    const bool stopPreset = io.write && io.write(0x0110, 2);
    const bool speedZero = io.write && io.write(0x0098, 0);
    view_.restoreAcknowledged = sonOff && stopPreset && speedZero;
    return view_.restoreAcknowledged;
}
void Probe::fail(const char* reason, const Io& io) {
    view_.reason = reason;
    if(!restore(io) && view_.enableAttempted) view_.reason = "restore_unconfirmed_use_main_switch";
    view_.phase = Phase::Failed;
}
bool Probe::cancel(const Io& io) {
    if(!active() && (!view_.enableAttempted || view_.restoreAcknowledged)) return true;
    // A failed restoration can be retried after the serial link returns.
    const bool stopped = restore(io);
    view_.phase = stopped ? Phase::Cancelled : Phase::Failed;
    view_.reason = stopped ? "cancelled" : "restore_unconfirmed_use_main_switch";
    return stopped;
}
void Probe::tick(uint32_t now, const Io& io) {
    if(!active()) return;
    if(static_cast<uint32_t>(now - view_.startedAt) >= kDeadlineMs) { fail("operator_deadline", io); return; }
    if(polled_ && static_cast<uint32_t>(now - view_.lastPollAt) < kPollMs) return;
    polled_ = true; view_.lastPollAt = now;
    Sample s = {};
    if(!sample(io, s)) {
        view_.latestValid = false;
        if(view_.phase == Phase::Preflight || view_.phase == Phase::VerifyHold || view_.phase == Phase::VerifyReturn) { fail("preflight_or_hold_read_failed", io); return; }
        if(view_.failedPolls == 0) view_.gapStartedAt = now;
        if(view_.failedPolls < std::numeric_limits<uint16_t>::max()) ++view_.failedPolls;
        if(view_.failedPolls >= 2 && static_cast<uint32_t>(now - view_.gapStartedAt) >= kGapMs) {
            view_.communicationGapObserved = true;
            view_.phase = Phase::AwaitReturn; view_.reason = "read_only_during_gap";
        }
        return; // No writes while waiting for the operator's power cycle.
    }
    view_.latest = s; view_.hasLatest = true; view_.latestValid = true;
    if(view_.phase == Phase::Preflight) {
        if(const char* why = invalid(s, 0)) { fail(why, io); return; }
        if(static_cast<int16_t>(s.source[2]) != 0) { fail("runtime_speed_not_zero", io); return; }
        if(!io.write || !io.write(0x0098, 0) || !io.write(0x0110, 2)) { fail("stop_preparation_failed", io); return; }
        view_.enableAttempted = true; // an unacknowledged write may still take effect
        if(!io.write(0x010F, 1)) { fail("zero_hold_enable_unconfirmed", io); return; }
        view_.phase = Phase::VerifyHold; view_.reason = "verifying_son_at_zero_preset"; return;
    }
    if(view_.phase == Phase::VerifyHold || view_.phase == Phase::WaitingForGap) {
        if(const char* why = invalid(s, 1)) { fail(why, io); return; }
        view_.before = s; view_.holdVerified = true;
        view_.failedPolls = 0; view_.phase = Phase::WaitingForGap;
        view_.reason = "zero_hold_verified_wait_for_operator_power_off"; return;
    }
    // First complete sample after the observed communications gap is retained
    // before any restoration write. A gap alone is NOT proof of a mains cycle.
    if(view_.phase == Phase::AwaitReturn) {
        view_.after = s; view_.afterGapCaptured = true; view_.capturedAt = now;
        view_.returnSamples = 0; view_.phase = Phase::VerifyReturn;
        view_.reason = "checking_post_gap_stability";
    }
    const char* why = s.forced[0] <= 1 ? invalid(s, s.forced[0]) : "invalid_son_after_gap";
    if(why) { fail(why, io); return; }
    view_.settledAfter = s;
    if(view_.returnSamples < std::numeric_limits<uint16_t>::max()) ++view_.returnSamples;
    if(s.forced[0] != view_.after.forced[0]) view_.sonConsistentAfterGap = false;
    if(view_.returnSamples < 3 || static_cast<uint32_t>(now - view_.capturedAt) < kReturnObservationMs) return;
    view_.returnVerified = true;
    if(!restore(io)) { fail("restore_unconfirmed_use_main_switch", io); return; }
    view_.phase = Phase::Complete;
    view_.reason = !view_.sonConsistentAfterGap ? "son_changed_during_boot" :
        s.forced[0] == 0 ? "son_reset_observed_after_gap" : "son_retained_observed_after_gap";
}
} // namespace kfm_zero_probe
