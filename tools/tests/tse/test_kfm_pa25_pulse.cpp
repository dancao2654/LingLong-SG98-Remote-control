#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include "TSE/kfm_pa25_pulse.h"
using namespace kfm_pa25_pulse;
namespace {
Sample model{};uint32_t clockMs=0,readDelay=0;
bool connected=true,ignoreClear=false,loseEnableAck=false,loseSpeedAck=false,coasting=false;
std::vector<std::pair<uint16_t,uint16_t>> writes;
bool readWords(uint16_t reg,uint16_t n,uint16_t* out){
 clockMs+=readDelay;if(!connected)return false;const uint16_t* words=nullptr;uint16_t rpm=static_cast<uint16_t>(model.rpm);
 switch(reg){case 22:assert(n==4);words=model.source;break;case 0x10F:assert(n==5);words=model.forced;break;case 0x100:assert(n==4);words=model.inputs;break;case 0x11E:assert(n==2);words=model.virtualInputs;break;case 4:words=&model.mode;break;case 53:words=&model.softwareEnable;break;case 0x1000:words=&rpm;break;case 0x1013:words=&model.alarm;break;case 0x100F:words=&model.inputsStatusRaw;break;case 0x1012:words=&model.busVoltageRaw;break;default:assert(false);}
 for(unsigned i=0;i<n;++i)out[i]=words[i];return true;
}
bool writeWord(uint16_t reg,uint16_t value){
 writes.emplace_back(reg,value);assert(reg==0x99||reg==0x10F||reg==0x98||reg==0x110);if(!connected)return false;
 if(reg==0x99){assert(value==0||value==static_cast<uint16_t>(kTargetRpm));if(value==0&&ignoreClear)return true;model.source[3]=value;if(value!=0&&loseSpeedAck)return false;}
 if(reg==0x10F){assert(value<=1);model.forced[0]=value;if(value==0&&!coasting)model.rpm=0;if(value==1&&loseEnableAck)return false;}
 if(reg==0x98){assert(value==0);model.source[2]=0;}
 if(reg==0x110){assert(value==2);model.forced[1]=2;}
 return true;
}
uint32_t now(){return clockMs;}const Io io={readWords,writeWord,now};
void fixture(){model={};model.mode=1;model.source[0]=1;model.source[1]=5000;model.forced[1]=2;for(unsigned i=0;i<4;++i)model.inputs[i]=i+1;clockMs=0;readDelay=0;connected=true;ignoreClear=false;loseEnableAck=false;loseSpeedAck=false;coasting=false;writes.clear();}
void tick(Pulse& p,uint32_t at){clockMs=at;p.tick(at,io);}
void run(Pulse& p){assert(p.start(0));tick(p,0);assert(p.view().phase==Phase::VerifyZeroEnable&&model.forced[0]==1&&model.source[3]==0);tick(p,100);assert(p.view().phase==Phase::Running&&p.view().zeroEnableVerified&&model.source[3]==static_cast<uint16_t>(-100));}
}
int main(){
 fixture();Pulse p;run(p);model.rpm=-95;tick(p,500);assert(p.view().motionObserved&&p.view().peakAbsRpm==95);tick(p,2100);
 assert(p.view().phase==Phase::Complete&&p.view().restoreAcknowledged&&p.view().restoreReadbackValid&&model.source[3]==0&&model.forced[0]==0);
 auto finishedWrites=writes.size();tick(p,9000);assert(writes.size()==finishedWrites);
 fixture();Pulse still;run(still);tick(still,2100);assert(still.view().phase==Phase::Complete&&!still.view().motionObserved&&std::strcmp(still.view().reason,"pulse_completed_no_motion_observed")==0);
 for(unsigned bad=0;bad<17;++bad){
  fixture();switch(bad){case 0:model.source[0]=0;break;case 1:model.mode=0;break;case 2:model.source[1]=499;break;case 3:model.source[2]=static_cast<uint16_t>(-1000);break;case 4:model.source[3]=1;break;case 5:model.softwareEnable=1;break;case 6:model.forced[0]=1;break;case 7:model.forced[1]=0;break;case 8:model.forced[2]=1;break;case 9:model.forced[3]=1;break;case 10:model.forced[4]=1;break;case 11:model.inputs[1]=0;break;case 12:model.virtualInputs[0]=1;break;case 13:model.inputsStatusRaw=1;break;case 14:model.rpm=1;break;case 15:model.alarm=3;break;case 16:connected=false;break;}
  Pulse badPulse;assert(badPulse.start(0));tick(badPulse,0);assert(badPulse.view().phase==Phase::Failed&&writes.empty());
 }
 fixture();Pulse enableLost;loseEnableAck=true;assert(enableLost.start(0));tick(enableLost,0);assert(enableLost.view().phase==Phase::Failed&&!enableLost.view().nonzeroAttempted&&enableLost.view().restoreReadbackValid&&model.forced[0]==0);
 fixture();Pulse speedLost;loseSpeedAck=true;assert(speedLost.start(0));tick(speedLost,0);tick(speedLost,100);assert(speedLost.view().phase==Phase::Failed&&speedLost.view().restoreReadbackValid&&model.source[3]==0);
 fixture();Pulse wrongHold;assert(wrongHold.start(0));tick(wrongHold,0);model.rpm=1;tick(wrongHold,100);assert(!wrongHold.view().nonzeroAttempted&&wrongHold.view().phase==Phase::Failed);
 for(int rpm:{-151,151,100,-32768}){fixture();Pulse wrongSpeed;run(wrongSpeed);model.rpm=rpm;tick(wrongSpeed,500);assert(wrongSpeed.view().phase==Phase::Failed&&model.source[3]==0);}
 fixture();Pulse fault;run(fault);model.alarm=7;tick(fault,500);assert(fault.view().phase==Phase::Failed&&model.source[3]==0&&model.alarm==7&&!fault.view().restoreReadbackValid);
 fixture();Pulse lost;run(lost);connected=false;auto count=writes.size();tick(lost,500);assert(lost.view().phase==Phase::Failed&&writes.size()==count+4);assert(!lost.start(700));connected=true;assert(lost.cancel(io));
 fixture();Pulse ignored;run(ignored);ignoreClear=true;assert(!ignored.cancel(io));assert(ignored.view().restoreAcknowledged&&!ignored.view().restoreReadbackValid&&!ignored.start(700));ignoreClear=false;assert(ignored.cancel(io));
 fixture();Pulse coast;run(coast);coasting=true;model.rpm=-20;assert(!coast.cancel(io));assert(coast.view().restoreAcknowledged&&!coast.view().restoreReadbackValid);model.rpm=0;assert(coast.cancel(io));
 fixture();Pulse slowPreflight;assert(slowPreflight.start(0));readDelay=1100;tick(slowPreflight,0);assert(slowPreflight.view().phase==Phase::Failed&&writes.empty());
 fixture();Pulse slowHold;assert(slowHold.start(0));tick(slowHold,0);readDelay=1100;tick(slowHold,100);assert(!slowHold.view().nonzeroAttempted&&model.forced[0]==0);
 fixture();Pulse bounded;run(bounded);model.rpm=-90;readDelay=220;tick(bounded,200);assert(bounded.view().phase==Phase::Complete&&model.forced[0]==0); // serial time is included
 fixture();Pulse normalCoast;run(normalCoast);coasting=true;model.rpm=-70;tick(normalCoast,2100);
 assert(normalCoast.view().phase==Phase::Stopping&&normalCoast.view().restoreAcknowledged&&!normalCoast.view().restoreReadbackValid);
 model.rpm=0;tick(normalCoast,2200);assert(normalCoast.view().phase==Phase::Complete&&normalCoast.view().restoreReadbackValid);
 fixture();Pulse stopTimeout;run(stopTimeout);coasting=true;model.rpm=-70;tick(stopTimeout,2100);tick(stopTimeout,5100);
 assert(stopTimeout.view().phase==Phase::Failed&&!stopTimeout.view().restoreReadbackValid);
 fixture();Pulse resetSession;model.source[3]=static_cast<uint16_t>(-100);model.forced[0]=1;model.rpm=-100;assert(resetSession.recover(io));assert(resetSession.view().restoreReadbackValid&&model.source[3]==0&&model.rpm==0);
 fixture();Pulse wrongRecovery;model.source[3]=1000;assert(!wrongRecovery.recover(io)&&writes.empty());
 fixture();Pulse active;run(active);count=writes.size();assert(!active.recover(io)&&writes.size()==count);assert(active.cancel(io));
 fixture();Pulse wrap;clockMs=UINT32_MAX-100;assert(wrap.start(clockMs));wrap.tick(clockMs,io);clockMs+=100;wrap.tick(clockMs,io);clockMs+=2000;wrap.tick(clockMs,io);assert(wrap.view().phase==Phase::Complete);
 puts("Fixed PA25 pulse and stop/recovery invariants passed");
}
