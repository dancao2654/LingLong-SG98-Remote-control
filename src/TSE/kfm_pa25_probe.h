#pragma once
#include "kfm_zero_probe.h"
namespace kfm_pa25_probe {
constexpr uint32_t kDeadlineMs=180000;
constexpr uint16_t kMarker=1;
using Phase=kfm_zero_probe::Phase;
using Io=kfm_zero_probe::Io;
struct Sample { uint16_t source[4]={}; uint16_t forced[5]={}; uint16_t inputs[4]={}; uint16_t virtualInputs[2]={}; uint16_t mode=0,softwareEnable=0,alarm=0,inputsStatusRaw=0,busVoltageRaw=0; int16_t rpm=0; };
struct View {
 Phase phase=Phase::Idle; const char* reason="not_started";
 uint32_t startedAt=0,lastPollAt=0,gapStartedAt=0,capturedAt=0;
 uint16_t failedPolls=0,returnSamples=0;
 bool writeAttempted=false,holdVerified=false,communicationGapObserved=false,afterGapCaptured=false,returnVerified=false,presetConsistent=true,restoreAcknowledged=false,restoreReadbackValid=false,hasLatest=false,latestValid=false;
 Sample latest={},before={},after={},settledAfter={};
};
class Probe {
public:
 bool start(uint32_t now); void tick(uint32_t now,const Io& io); bool cancel(const Io& io);
 bool recover(const Io& io);
 bool active() const; const View& view()const{return v_;}
private:
 bool sample(const Io&,Sample&); const char* invalid(const Sample&)const;
 bool restore(const Io&); void fail(const char*,const Io&);
 View v_={}; bool polled_=false;
};
}
