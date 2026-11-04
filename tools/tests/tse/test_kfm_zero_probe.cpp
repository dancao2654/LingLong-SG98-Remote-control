#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
#include <cstring>
#include "TSE/kfm_zero_probe.h"
using namespace kfm_zero_probe;
namespace {
Sample model{};
bool connected=true, loseEnableAck=false, refuseSonOff=false;
std::vector<std::pair<uint16_t,uint16_t>> writes;
bool readWords(uint16_t reg,uint16_t n,uint16_t* out){
 if(!connected)return false;
 const uint16_t* words=nullptr;uint16_t rpm=static_cast<uint16_t>(model.rpm);
 switch(reg){
  case 22:assert(n==4);words=model.source;break;
  case 0x10F:assert(n==5);words=model.forced;break;
  case 0x100:assert(n==4);words=model.inputs;break;
  case 0x11E:assert(n==2);words=model.virtualInputs;break;
  case 4:words=&model.mode;break;
  case 53:words=&model.softwareEnable;break;
  case 0x1000:words=&rpm;break;
  case 0x1013:words=&model.alarm;break;
  case 0x1012:words=&model.busVoltageRaw;break;
  case 0x1016:words=&model.operatingStatusRaw;break;
  case 0x100F:words=&model.inputsStatusRaw;break;
  default:assert(false);
 }
 for(uint16_t i=0;i<n;++i)out[i]=words[i];return true;
}
bool writeWord(uint16_t reg,uint16_t v){
 writes.emplace_back(reg,v);
 if(reg==0x98){assert(v==0);model.source[2]=0;}
 else if(reg==0x110){assert(v==2);model.forced[1]=2;}
 else if(reg==0x10F){assert(v<=1);if(v==0&&refuseSonOff)return false;model.forced[0]=v;if(v==1&&loseEnableAck)return false;}
 else assert(false);
 return connected;
}
const Io io={readWords,writeWord};
void fixture(){
 model={};model.source[0]=1;model.source[1]=5000;model.forced[1]=2;model.mode=1;
 for(uint16_t i=0;i<4;++i)model.inputs[i]=i+1;
 model.busVoltageRaw=325;model.operatingStatusRaw=1;
 connected=true;loseEnableAck=false;refuseSonOff=false;writes.clear();
}
void hold(Probe& p,uint32_t now=0){assert(p.start(now));p.tick(now,io);p.tick(now+250,io);assert(p.view().holdVerified);assert(p.view().before.forced[0]==1);}
void gap(Probe& p,uint32_t now=500){
 connected=false;auto count=writes.size();p.tick(now,io);p.tick(now+250,io);p.tick(now+500,io);
 assert(p.view().communicationGapObserved&&p.view().phase==Phase::AwaitReturn);
 assert(writes.size()==count); // observes driver-off without overwriting the return state
}
}
int main(){
 for(uint16_t son:{0,1}){
  fixture();Probe bootChange;hold(bootChange);gap(bootChange);connected=true;model.forced[0]=0;
 bootChange.tick(1250,io);model.forced[0]=1;
 for(uint32_t t=1500;t<=3500&&bootChange.active();t+=250)bootChange.tick(t,io);
 assert(bootChange.view().returnVerified&&!bootChange.view().sonConsistentAfterGap);
 assert(std::strcmp(bootChange.view().reason,"son_changed_during_boot")==0);
 assert(model.forced[0]==0);
 fixture();Probe p;hold(p);gap(p);connected=true;model.forced[0]=son;model.source[2]=static_cast<uint16_t>(-1000);
  p.tick(1250,io);assert(p.view().afterGapCaptured&&!p.view().restoreAttempted);
  for(uint32_t t=1500;t<=3500&&p.active();t+=250)p.tick(t,io);
  assert(p.view().phase==Phase::Complete&&p.view().returnVerified&&p.view().restoreAcknowledged);
  assert(p.view().after.forced[0]==son&&static_cast<int16_t>(p.view().after.source[2])==-1000);
  assert(model.forced[0]==0&&model.forced[1]==2&&model.source[2]==0);
 }
 // No enable is allowed with any incompatible zero-preset source/input state.
 for(int bad=0;bad<15;++bad){
  fixture();switch(bad){
   case 0:model.source[0]=0;break;case 1:model.source[3]=100;break;case 2:model.mode=0;break;
   case 3:model.forced[0]=1;break;case 4:model.forced[1]=0;break;case 5:model.forced[2]=1;break;
   case 6:model.forced[3]=1;break;case 7:model.forced[4]=1;break;case 8:model.inputs[0]=0;break;
   case 9:model.virtualInputs[0]=1;break;case 10:model.virtualInputs[1]=1;break;
   case 11:model.rpm=3;break;case 12:model.alarm=7;break;case 13:model.source[2]=static_cast<uint16_t>(-1000);break;case 14:model.softwareEnable=1;break;
  }
  Probe p;assert(p.start(0));p.tick(0,io);assert(p.view().phase==Phase::Failed&&writes.empty());
 }
 fixture();Probe p;hold(p);assert(!p.start(100));assert(p.cancel(io));assert(!p.active()&&model.forced[0]==0);
 fixture();Probe unacked;loseEnableAck=true;assert(unacked.start(0));unacked.tick(0,io);
 assert(unacked.view().phase==Phase::Failed&&unacked.view().enableAttempted&&unacked.view().restoreAcknowledged);
 assert(model.forced[0]==0&&model.forced[1]==2);
 fixture();Probe lost;hold(lost);connected=false;lost.tick(500,io);connected=true;lost.tick(750,io);
 assert(!lost.view().communicationGapObserved&&lost.active()); // one missing poll is not a mains-cycle proof
 assert(lost.cancel(io));
 fixture();Probe timeout;hold(timeout);timeout.tick(kDeadlineMs,io);
 assert(timeout.view().phase==Phase::Failed&&model.forced[0]==0&&timeout.view().restoreAcknowledged);
 fixture();Probe drift;hold(drift);model.source[3]=100;drift.tick(500,io);
 assert(drift.view().phase==Phase::Failed&&model.forced[0]==0);
 fixture();Probe rotation;hold(rotation);model.rpm=5;rotation.tick(500,io);
 assert(rotation.view().phase==Phase::Failed&&rotation.view().latest.rpm==5&&model.forced[0]==0);
 fixture();Probe failStop;hold(failStop);refuseSonOff=true;assert(!failStop.cancel(io));
 assert(failStop.view().phase==Phase::Failed&&std::strstr(failStop.view().reason,"main_switch"));
 refuseSonOff=false;assert(failStop.cancel(io));assert(model.forced[0]==0&&failStop.view().restoreAcknowledged);
 fixture();Probe wrap;hold(wrap,UINT32_MAX-100);assert(wrap.active());assert(wrap.cancel(io));
 fixture();Probe readFail;connected=false;assert(readFail.start(0));readFail.tick(0,io);
 assert(readFail.view().phase==Phase::Failed&&writes.empty());
 std::puts("KFM zero-preset probe invariants passed");
}
