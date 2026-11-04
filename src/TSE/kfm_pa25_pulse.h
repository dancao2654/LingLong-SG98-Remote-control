#pragma once
#include "kfm_pa25_probe.h"
namespace kfm_pa25_pulse {
constexpr int16_t kTargetRpm=-100;
constexpr uint16_t kMaxObservedRpm=150;
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
constexpr uint32_t kRunMs=5000;
#else
constexpr uint32_t kRunMs=2000;
#endif
constexpr uint32_t kDeadlineMs=10000, kStopMs=3000;
using Sample=kfm_pa25_probe::Sample;
enum class Phase:uint8_t {Idle,Preflight,VerifyZeroEnable,Running,Stopping,Complete,Failed,Cancelled};
struct Io {
 bool (*read)(uint16_t,uint16_t,uint16_t*);
 bool (*write)(uint16_t,uint16_t);
 uint32_t (*now)();
};
struct View {
 Phase phase=Phase::Idle;const char* reason="not_started";
 uint32_t startedAt=0,runStartedAt=0,stopStartedAt=0,lastPollAt=0;
 bool writeAttempted=false,nonzeroAttempted=false,zeroEnableVerified=false;
 bool restoreAcknowledged=false,restoreReadbackValid=false,latestValid=false;
 bool motionObserved=false;uint16_t peakAbsRpm=0;
 Sample latest={};
};
const char* phaseName(Phase);
class Pulse {
public:
 bool start(uint32_t);void tick(uint32_t,const Io&);bool cancel(const Io&);bool recover(const Io&);
 bool active()const;const View& view()const{return v_;}
private:
 bool sample(const Io&,Sample&);const char* invalid(const Sample&,uint16_t,uint16_t,bool)const;
 bool restore(const Io&);void finish(const char*,bool,const Io&);
 View v_={};bool polled_=false;
};
}
