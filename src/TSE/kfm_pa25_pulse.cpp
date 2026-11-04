#include "kfm_pa25_pulse.h"
namespace kfm_pa25_pulse {
const char* phaseName(Phase p){switch(p){case Phase::Idle:return "idle";case Phase::Preflight:return "preflight";case Phase::VerifyZeroEnable:return "verify_zero_enable";case Phase::Running:return "running";case Phase::Stopping:return "stopping";case Phase::Complete:return "complete";case Phase::Failed:return "failed";case Phase::Cancelled:return "cancelled";}return "unknown";}
bool Pulse::active()const{return v_.phase==Phase::Preflight||v_.phase==Phase::VerifyZeroEnable||v_.phase==Phase::Running||v_.phase==Phase::Stopping;}
bool Pulse::start(uint32_t now){
 if(active()||(v_.writeAttempted&&(!v_.restoreAcknowledged||!v_.restoreReadbackValid)))return false;
 v_={};v_.startedAt=now;v_.phase=Phase::Preflight;v_.reason="checking_stationary_zero_presets";polled_=false;return true;
}
bool Pulse::sample(const Io& io,Sample& out){
 if(!io.read)return false;Sample s={};uint16_t rpm=0;
 if(!io.read(22,4,s.source)||!io.read(0x10F,5,s.forced)||!io.read(0x100,4,s.inputs)||!io.read(0x11E,2,s.virtualInputs)||!io.read(4,1,&s.mode)||!io.read(53,1,&s.softwareEnable)||!io.read(0x1000,1,&rpm)||!io.read(0x1013,1,&s.alarm)||!io.read(0x100F,1,&s.inputsStatusRaw)||!io.read(0x1012,1,&s.busVoltageRaw))return false;
 s.rpm=static_cast<int16_t>(rpm);out=s;return true;
}
const char* Pulse::invalid(const Sample& s,uint16_t son,uint16_t speed,bool stationary)const{
 if(s.source[0]!=1||s.source[1]<500||s.mode!=1)return "speed_mode_or_limit_mismatch";
 if(s.source[2]!=0||s.source[3]!=speed)return "runtime_preset_mismatch";
 if(s.softwareEnable!=0||s.forced[0]!=son||s.forced[1]!=2||s.forced[2]||s.forced[3]||s.forced[4])return "enable_or_selection_mismatch";
 for(unsigned i=0;i<4;++i)if(s.inputs[i]!=i+1)return "factory_input_mapping_mismatch";
 if(s.virtualInputs[0]||s.virtualInputs[1]||s.inputsStatusRaw)return "input_status_not_zero";
 if(s.alarm)return "drive_alarm";
 const int32_t rpm=s.rpm;const int32_t magnitude=rpm<0?-rpm:rpm;
 if(stationary&&rpm)return "not_stationary";
 if(magnitude>kMaxObservedRpm)return "rpm_limit_exceeded";
 if(!stationary&&rpm>5)return "unexpected_rpm_sign";
 return nullptr;
}
bool Pulse::restore(const Io& io){
 if(!v_.writeAttempted)return true;
 const bool a=io.write&&io.write(0x10F,0);
 const bool b=io.write&&io.write(0x99,0);
 const bool c=io.write&&io.write(0x98,0);
 const bool d=io.write&&io.write(0x110,2);
 v_.restoreAcknowledged=a&&b&&c&&d;
 Sample s={};v_.latestValid=sample(io,s);v_.restoreReadbackValid=false;
 if(v_.latestValid){v_.latest=s;v_.restoreReadbackValid=invalid(s,0,0,true)==nullptr;}
 return v_.restoreAcknowledged&&v_.restoreReadbackValid;
}
void Pulse::finish(const char* reason,bool success,const Io& io){
 v_.stopStartedAt=io.now?io.now():v_.lastPollAt;
 const bool stopped=restore(io);
 if(success&&!stopped&&v_.restoreAcknowledged&&v_.latestValid&&invalid(v_.latest,0,0,false)==nullptr){
  v_.phase=Phase::Stopping;v_.reason="waiting_for_stationary_stop";return;
 }
 v_.phase=success&&stopped?Phase::Complete:Phase::Failed;
 v_.reason=!stopped&&v_.writeAttempted?"stop_unconfirmed_use_main_switch":reason;
}
bool Pulse::cancel(const Io& io){
 if(!active()&&(!v_.writeAttempted||(v_.restoreAcknowledged&&v_.restoreReadbackValid)))return true;
 const bool ok=restore(io);v_.phase=ok?Phase::Cancelled:Phase::Failed;v_.reason=ok?"cancelled":"stop_unconfirmed_use_main_switch";return ok;
}
bool Pulse::recover(const Io& io){
 if(active())return false;v_.latestValid=false;v_.restoreReadbackValid=false;
 Sample s={};if(!sample(io,s)){v_.reason="recovery_read_failed";return false;}
 v_.latest=s;v_.latestValid=true;
 bool compatible=s.mode==1&&s.source[0]==1&&(s.source[3]==0||s.source[3]==static_cast<uint16_t>(kTargetRpm))&&s.softwareEnable==0&&s.forced[0]<=1&&s.forced[1]==2&&!s.forced[2]&&!s.forced[3]&&!s.forced[4]&&!s.virtualInputs[0]&&!s.virtualInputs[1]&&!s.inputsStatusRaw;
 for(unsigned i=0;i<4;++i)compatible=compatible&&s.inputs[i]==i+1;
 if(!compatible){v_.reason="recovery_configuration_mismatch";return false;}
 v_.writeAttempted=true;const bool ok=restore(io);v_.phase=ok?Phase::Cancelled:Phase::Failed;
 v_.reason=ok?"explicit_recovery_verified":"stop_unconfirmed_use_main_switch";return ok;
}
void Pulse::tick(uint32_t now,const Io& io){
 if(!active())return;
 now=io.now?io.now():now;
 if(v_.phase==Phase::Stopping){
  if(static_cast<uint32_t>(now-v_.stopStartedAt)>=kStopMs){finish("stop_deadline",false,io);return;}
  if(polled_&&static_cast<uint32_t>(now-v_.lastPollAt)<100)return;
  polled_=true;v_.lastPollAt=now;Sample stopped={};
  if(!sample(io,stopped)){v_.latestValid=false;finish("stop_read_failed",false,io);return;}
  v_.latest=stopped;v_.latestValid=true;
  if(const char* why=invalid(stopped,0,0,false)){finish(why,false,io);return;}
  if(stopped.rpm==0){v_.restoreReadbackValid=true;v_.phase=Phase::Complete;v_.reason=v_.motionObserved?"pulse_completed_motion_observed":"pulse_completed_no_motion_observed";}
  return;
 }
 if(v_.nonzeroAttempted&&static_cast<uint32_t>(now-v_.runStartedAt)>=kRunMs){finish(v_.motionObserved?"pulse_completed_motion_observed":"pulse_completed_no_motion_observed",true,io);return;}
 if(static_cast<uint32_t>(now-v_.startedAt)>=kDeadlineMs){finish("overall_deadline",false,io);return;}
 if(polled_&&static_cast<uint32_t>(now-v_.lastPollAt)<100)return;
 polled_=true;v_.lastPollAt=now;Sample s={};
 if(!sample(io,s)){v_.latestValid=false;finish("required_read_failed",false,io);return;}
 v_.latest=s;v_.latestValid=true;
 const int32_t rpm=s.rpm;const uint16_t magnitude=static_cast<uint16_t>(rpm<0?-rpm:rpm);
 if(magnitude>v_.peakAbsRpm)v_.peakAbsRpm=magnitude;
 if(rpm!=0&&v_.nonzeroAttempted)v_.motionObserved=true;
 const uint16_t son=v_.phase==Phase::Preflight?0:1;
 const uint16_t speed=v_.phase==Phase::Running?static_cast<uint16_t>(kTargetRpm):0;
 if(const char* why=invalid(s,son,speed,v_.phase!=Phase::Running)){finish(why,false,io);return;}
 now=io.now?io.now():now;
 // Blocking serial work is included; never issue the speed command after an
 // expired preflight/hold deadline, or extend a pulse with a stale tick time.
 if(static_cast<uint32_t>(now-v_.startedAt)>=kDeadlineMs){finish("overall_deadline",false,io);return;}
 if(v_.phase==Phase::Preflight){
  v_.writeAttempted=true;
  if(!io.write||!io.write(0x10F,1)){finish("zero_enable_unconfirmed",false,io);return;}
  v_.phase=Phase::VerifyZeroEnable;v_.reason="verifying_enable_at_zero";return;
 }
 if(v_.phase==Phase::VerifyZeroEnable){
  v_.zeroEnableVerified=true;v_.nonzeroAttempted=true;v_.runStartedAt=now;
  if(!io.write||!io.write(0x99,static_cast<uint16_t>(kTargetRpm))){finish("speed_write_unconfirmed",false,io);return;}
  v_.phase=Phase::Running;v_.reason="fixed_100rpm_pulse";return;
 }
 if(static_cast<uint32_t>(now-v_.runStartedAt)>=kRunMs)finish(v_.motionObserved?"pulse_completed_motion_observed":"pulse_completed_no_motion_observed",true,io);
}
}
