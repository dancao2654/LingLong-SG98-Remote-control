#include "kfm_pa25_probe.h"
namespace kfm_pa25_probe {
bool Probe::active()const{return v_.phase==Phase::Preflight||v_.phase==Phase::VerifyHold||v_.phase==Phase::WaitingForGap||v_.phase==Phase::AwaitReturn||v_.phase==Phase::VerifyReturn;}
bool Probe::start(uint32_t now){if(active()||(v_.writeAttempted&&(!v_.restoreAcknowledged||!v_.restoreReadbackValid)))return false;v_={};v_.startedAt=now;v_.phase=Phase::Preflight;v_.reason="checking_disabled_factory_state";polled_=false;return true;}
bool Probe::sample(const Io& io,Sample& out){
 if(!io.read)return false; Sample s={};uint16_t rpm=0;
 if(!io.read(22,4,s.source)||!io.read(0x10F,5,s.forced)||!io.read(0x100,4,s.inputs)||!io.read(0x11E,2,s.virtualInputs)||!io.read(4,1,&s.mode)||!io.read(53,1,&s.softwareEnable)||!io.read(0x1000,1,&rpm)||!io.read(0x1013,1,&s.alarm)||!io.read(0x100F,1,&s.inputsStatusRaw)||!io.read(0x1012,1,&s.busVoltageRaw))return false;
 s.rpm=static_cast<int16_t>(rpm);out=s;return true;
}
const char* Probe::invalid(const Sample& s)const{
 if(s.source[0]!=1||s.mode!=1||s.source[1]<500)return "speed_mode_or_limit_mismatch";
 if(s.softwareEnable!=0||s.forced[0]!=0)return "enable_not_off";
 if(s.forced[1]!=2||s.forced[2]||s.forced[3]||s.forced[4])return "forced_input_mismatch";
 for(unsigned i=0;i<4;++i)if(s.inputs[i]!=i+1)return "factory_input_mapping_mismatch";
 if(s.virtualInputs[0]||s.virtualInputs[1]||s.inputsStatusRaw)return "input_status_not_zero";
 if(s.rpm)return "unexpected_rotation";if(s.alarm)return "drive_alarm";return nullptr;
}
bool Probe::restore(const Io& io){
 if(!v_.writeAttempted)return true;
 // All commands are attempted. Never enable or write EEPROM. Clear the marker
 // before confirming the normal zero-preset selection.
 const bool a=io.write&&io.write(0x10F,0);
 const bool b=io.write&&io.write(0x99,0);
 const bool c=io.write&&io.write(0x98,0);
 const bool d=io.write&&io.write(0x110,2);
 v_.restoreAcknowledged=a&&b&&c&&d;
 Sample readback={};
 v_.restoreReadbackValid=sample(io,readback);
 if(v_.restoreReadbackValid){
  v_.latest=readback;v_.hasLatest=true;v_.latestValid=true;
  v_.restoreReadbackValid=invalid(readback)==nullptr && readback.source[2]==0 && readback.source[3]==0;
 }else v_.latestValid=false;
 return v_.restoreAcknowledged&&v_.restoreReadbackValid;
}
// Explicit zero-only recovery also works after the controller lost its RAM
// session. It does not infer ownership from an empty local Probe object.
bool Probe::recover(const Io& io){
 if(active())return false;
 v_.latestValid=false;
 v_.restoreReadbackValid=false;
 Sample before={};
 if(!sample(io,before)){v_.reason="recovery_read_failed";return false;}
 v_.latest=before;v_.hasLatest=true;v_.latestValid=true;
 // Restrict recovery to the known factory topology and possible audit marker.
 // SON may be 1 and RPM/alarm need not be zero: the goal is to attempt a stop,
 // never to clear an alarm or permit a new marker/run.
 bool compatible=before.mode==1 && before.source[0]==1 && before.source[3]<=kMarker &&
     before.softwareEnable==0 && before.forced[0]<=1 && before.forced[1]==2 &&
     before.forced[2]==0 && before.forced[3]==0 && before.forced[4]==0 &&
     before.virtualInputs[0]==0 && before.virtualInputs[1]==0 && before.inputsStatusRaw==0;
 for(unsigned i=0;i<4;++i)compatible=compatible&&before.inputs[i]==i+1;
 if(!compatible){v_.reason="recovery_configuration_mismatch";return false;}
 v_.writeAttempted=true; // cleanup owed even after a lost write echo
 const bool ok=restore(io);
 v_.phase=ok?Phase::Cancelled:Phase::Failed;
 v_.reason=ok?"explicit_recovery_verified":"recovery_unconfirmed_use_main_switch";
 return ok;
}
void Probe::fail(const char* why,const Io& io){v_.reason=why;if(!restore(io)&&v_.writeAttempted)v_.reason="restore_unconfirmed_use_main_switch";v_.phase=Phase::Failed;}
bool Probe::cancel(const Io& io){if(!active()&&(!v_.writeAttempted||(v_.restoreAcknowledged&&v_.restoreReadbackValid)))return true;const bool ok=restore(io);v_.phase=ok?Phase::Cancelled:Phase::Failed;v_.reason=ok?"cancelled":"restore_unconfirmed_use_main_switch";return ok;}
void Probe::tick(uint32_t now,const Io& io){
 if(!active())return;
 if(static_cast<uint32_t>(now-v_.startedAt)>=kDeadlineMs){fail("operator_deadline",io);return;}
 if(polled_&&static_cast<uint32_t>(now-v_.lastPollAt)<250)return;
 polled_=true;v_.lastPollAt=now;Sample s={};
 if(!sample(io,s)){
  v_.latestValid=false;
  if(v_.phase==Phase::Preflight||v_.phase==Phase::VerifyHold||v_.phase==Phase::VerifyReturn){fail("required_read_failed",io);return;}
  if(!v_.failedPolls)v_.gapStartedAt=now;if(v_.failedPolls<65535)++v_.failedPolls;
  if(v_.failedPolls>=2&&static_cast<uint32_t>(now-v_.gapStartedAt)>=500){v_.communicationGapObserved=true;v_.phase=Phase::AwaitReturn;v_.reason="read_only_during_gap";}
  return;
 }
 v_.latest=s;v_.hasLatest=true;v_.latestValid=true;
 // Freeze the first returned sample even when it reveals a failed guard.
 if(v_.phase==Phase::AwaitReturn){v_.after=s;v_.afterGapCaptured=true;v_.capturedAt=now;v_.phase=Phase::VerifyReturn;v_.reason="checking_pa25_after_gap";}
 if(const char* why=invalid(s)){fail(why,io);return;}
 if(v_.phase==Phase::Preflight){
  if(s.source[3]!=0||s.source[2]!=0){fail("runtime_presets_not_zero",io);return;}
  // Mark BEFORE sending: a lost echo must still trigger restoration.
  v_.writeAttempted=true;
  if(!io.write||!io.write(0x10F,0)||!io.write(0x99,kMarker)){fail("marker_write_unconfirmed",io);return;}
  v_.phase=Phase::VerifyHold;v_.reason="verifying_marker_while_disabled";return;
 }
 if(v_.phase==Phase::VerifyHold||v_.phase==Phase::WaitingForGap){
  if(s.source[3]!=kMarker||s.source[2]!=0){fail("marker_or_primary_preset_changed",io);return;}
  v_.before=s;v_.holdVerified=true;v_.failedPolls=0;v_.phase=Phase::WaitingForGap;v_.reason="marker_verified_wait_for_operator_power_off";return;
 }
 if(s.source[3]!=0&&s.source[3]!=kMarker){fail("unexpected_pa25_after_gap",io);return;}
 v_.settledAfter=s;if(v_.returnSamples<65535)++v_.returnSamples;
 if(s.source[3]!=v_.after.source[3])v_.presetConsistent=false;
 if(v_.returnSamples<3||static_cast<uint32_t>(now-v_.capturedAt)<2000)return;
 v_.returnVerified=true;
 if(!restore(io)){fail("restore_unconfirmed_use_main_switch",io);return;}
 v_.phase=Phase::Complete;v_.reason=!v_.presetConsistent?"pa25_changed_during_return":s.source[3]==0?"pa25_reset_to_zero_observed":"pa25_marker_retained";
}
}
