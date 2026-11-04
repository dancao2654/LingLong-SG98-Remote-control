#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include "TSE/kfm_pa25_probe.h"
using namespace kfm_pa25_probe;
namespace {
Sample model{};
bool connected=true,loseMarkerAck=false,refuseSonOff=false,refuseClear=false,ignoreClear=false;
std::vector<std::pair<uint16_t,uint16_t>> writes;
bool readWords(uint16_t reg,uint16_t n,uint16_t* out){
 if(!connected)return false;
 const uint16_t* words=nullptr;uint16_t rpm=static_cast<uint16_t>(model.rpm);
 switch(reg){
 case 22:assert(n==4);words=model.source;break;
 case 0x10F:assert(n==5);words=model.forced;break;
 case 0x100:assert(n==4);words=model.inputs;break;
 case 0x11E:assert(n==2);words=model.virtualInputs;break;
 case 4:words=&model.mode;break;case 53:words=&model.softwareEnable;break;
 case 0x1000:words=&rpm;break;case 0x1013:words=&model.alarm;break;
 case 0x100F:words=&model.inputsStatusRaw;break;case 0x1012:words=&model.busVoltageRaw;break;
 default:assert(false);
 }
 for(unsigned i=0;i<n;++i)out[i]=words[i];return true;
}
bool writeWord(uint16_t reg,uint16_t value){
 writes.emplace_back(reg,value);
 assert(reg==0x99||value==0||reg==0x110);
 if(!connected)return false;
 if(reg==0x99){assert(value<=1);if(!value&&refuseClear)return false;if(!value&&ignoreClear)return true;model.source[3]=value;if(value&&loseMarkerAck)return false;}
 else if(reg==0x98){assert(value==0);model.source[2]=0;}
 else if(reg==0x10F){assert(value==0);if(refuseSonOff)return false;model.forced[0]=0;}
 else if(reg==0x110){assert(value==2);model.forced[1]=2;}
 else assert(false);
 return true;
}
const Io io={readWords,writeWord};
void fixture(){model={};model.mode=1;model.source[0]=1;model.source[1]=5000;model.forced[1]=2;for(unsigned i=0;i<4;++i)model.inputs[i]=i+1;connected=true;loseMarkerAck=false;refuseSonOff=false;refuseClear=false;ignoreClear=false;writes.clear();}
void hold(Probe& p,uint32_t now=0){assert(p.start(now));p.tick(now,io);p.tick(now+250,io);assert(p.view().holdVerified&&model.forced[0]==0&&model.source[3]==1);}
void gap(Probe& p){connected=false;auto count=writes.size();p.tick(500,io);p.tick(750,io);p.tick(1000,io);assert(p.view().phase==Phase::AwaitReturn&&p.view().communicationGapObserved);assert(writes.size()==count);}
}
int main(){
 for(uint16_t returned:{0,1}){
  fixture();Probe p;hold(p);gap(p);connected=true;model.source[3]=returned;model.source[2]=static_cast<uint16_t>(-1000);
  p.tick(1250,io);assert(p.view().afterGapCaptured&&!p.view().restoreAcknowledged&&p.view().after.source[3]==returned);
  for(unsigned t=1500;t<=3500&&p.active();t+=250)p.tick(t,io);
  assert(p.view().phase==Phase::Complete&&p.view().returnVerified&&p.view().restoreAcknowledged&&p.view().presetConsistent);
  assert(model.source[3]==0&&model.source[2]==0&&model.forced[0]==0&&model.forced[1]==2);
  assert(std::strcmp(p.view().reason,returned?"pa25_marker_retained":"pa25_reset_to_zero_observed")==0);
 }
 for(unsigned bad=0;bad<18;++bad){
  fixture();switch(bad){
   case 0:model.source[0]=0;break;case 1:model.mode=0;break;case 2:model.source[1]=499;break;
   case 3:model.softwareEnable=1;break;case 4:model.forced[0]=1;break;case 5:model.forced[1]=0;break;
   case 6:model.forced[2]=1;break;case 7:model.forced[3]=1;break;case 8:model.forced[4]=1;break;
   case 9:model.inputs[0]=0;break;case 10:model.virtualInputs[0]=1;break;case 11:model.virtualInputs[1]=1;break;
   case 12:model.inputsStatusRaw=1;break;case 13:model.rpm=1;break;case 14:model.alarm=5;break;
   case 15:model.source[2]=static_cast<uint16_t>(-1000);break;case 16:model.source[3]=1;break;case 17:connected=false;break;
  }
  Probe p;assert(p.start(0));p.tick(0,io);assert(p.view().phase==Phase::Failed&&writes.empty());
 }
 fixture();Probe unacked;loseMarkerAck=true;assert(unacked.start(0));unacked.tick(0,io);assert(unacked.view().phase==Phase::Failed&&unacked.view().writeAttempted&&unacked.view().restoreAcknowledged&&model.source[3]==0);
 fixture();Probe lost;hold(lost);connected=false;auto count=writes.size();assert(!lost.cancel(io));assert(writes.size()==count+4);assert(!lost.start(3000));connected=true;assert(lost.cancel(io));assert(lost.view().restoreAcknowledged&&model.source[3]==0);
 fixture();Probe timeout;hold(timeout);timeout.tick(kDeadlineMs,io);assert(timeout.view().phase==Phase::Failed&&timeout.view().restoreAcknowledged);
 fixture();Probe transient;hold(transient);gap(transient);connected=true;model.source[3]=0;transient.tick(1250,io);model.source[3]=1;
 for(unsigned t=1500;t<=3500&&transient.active();t+=250)transient.tick(t,io);
 assert(transient.view().returnVerified&&!transient.view().presetConsistent&&std::strcmp(transient.view().reason,"pa25_changed_during_return")==0);
 fixture();Probe fault;hold(fault);gap(fault);connected=true;model.forced[0]=1;fault.tick(1250,io);
 assert(fault.view().afterGapCaptured&&fault.view().after.forced[0]==1&&fault.view().phase==Phase::Failed&&model.forced[0]==0);
 fixture();Probe rotate;hold(rotate);model.rpm=2;rotate.tick(500,io);assert(rotate.view().phase==Phase::Failed&&rotate.view().restoreAcknowledged);
 fixture();Probe badReturn;hold(badReturn);gap(badReturn);connected=true;model.source[3]=42;badReturn.tick(1250,io);assert(badReturn.view().phase==Phase::Failed&&badReturn.view().after.source[3]==42&&model.source[3]==0);
 fixture();Probe shortGap;hold(shortGap);connected=false;shortGap.tick(500,io);connected=true;shortGap.tick(750,io);assert(!shortGap.view().communicationGapObserved);assert(shortGap.cancel(io));
 fixture();Probe longGap;hold(longGap);gap(longGap);longGap.tick(90000,io);assert(longGap.active());connected=true;model.source[3]=0;for(unsigned t=90250;t<=92500&&longGap.active();t+=250)longGap.tick(t,io);assert(longGap.view().phase==Phase::Complete);
 fixture();Probe restoreFail;hold(restoreFail);refuseClear=true;assert(!restoreFail.cancel(io));assert(!restoreFail.start(1000));refuseClear=false;assert(restoreFail.cancel(io));
 fixture();Probe wrap;hold(wrap,UINT32_MAX-100);assert(wrap.cancel(io));

 // A controller reset loses local session ownership, but the marker can remain
 // in the servo. Normal start must refuse it; explicit recovery can clear it.
 fixture();Probe beforeReset;hold(beforeReset);Probe afterReset;
 assert(afterReset.start(1000));afterReset.tick(1000,io);
 assert(afterReset.view().phase==Phase::Failed&&model.source[3]==1);
 const auto recoveryWrites=writes.size();assert(afterReset.recover(io));
 assert(model.source[3]==0&&afterReset.view().restoreReadbackValid&&afterReset.view().restoreAcknowledged);
 for(size_t i=recoveryWrites;i<writes.size();++i)assert(writes[i].second==0||writes[i].first==0x110);
 fixture();Probe active;hold(active);auto unchanged=writes.size();assert(!active.recover(io)&&writes.size()==unchanged);assert(active.cancel(io));
 fixture();Probe wrong;model.source[3]=9;unchanged=writes.size();assert(!wrong.recover(io)&&writes.size()==unchanged);
 fixture();Probe badMapping;model.inputs[0]=9;assert(!badMapping.recover(io)&&writes.empty());
 fixture();Probe badSource;model.source[0]=0;assert(!badSource.recover(io)&&writes.empty());
 fixture();Probe inputActive;model.inputsStatusRaw=1;assert(!inputActive.recover(io)&&writes.empty());
 fixture();Probe noRead;connected=false;assert(!noRead.recover(io)&&writes.empty());
 fixture();Probe recoverEnabled;model.source[3]=1;model.forced[0]=1;assert(recoverEnabled.recover(io));assert(model.forced[0]==0&&model.source[3]==0);
 fixture();Probe alarmed;model.source[3]=1;model.alarm=7;assert(!alarmed.recover(io));assert(alarmed.view().restoreAcknowledged&&!alarmed.view().restoreReadbackValid&&model.source[3]==0&&model.alarm==7);
 fixture();Probe recoverLost;model.source[3]=1;refuseClear=true;assert(!recoverLost.recover(io));refuseClear=false;assert(recoverLost.recover(io));
 fixture();Probe falseEcho;model.source[3]=1;ignoreClear=true;assert(!falseEcho.recover(io));
 assert(falseEcho.view().restoreAcknowledged&&!falseEcho.view().restoreReadbackValid);
 assert(!falseEcho.start(500));assert(!falseEcho.cancel(io));ignoreClear=false;assert(falseEcho.cancel(io));
 puts("PA25 disabled-drive marker audit invariants passed");
}
