#include <cassert>
#include <cstdio>
#include <cstring>
#include "Arduino.h"
#include "comm.h"
#include "main.h"
#include "motor_hal.h"
#include "TSE/motorcontrol_tse.h"
#include "SEW/movilink.h"
unsigned long fakeNow=10000;
bool fakePeerActive=true,simulatePeerTraffic=false;
unsigned long fakePeerAt=10000;
FakePrint Serial;
FakeUart Serial2;
int16_t motor_setRPM=0,motor_currentRPM=0,motor_currentTorque=0,commCounter=0;
COMM_STATUS commStatus=COMM_DISCONNECTED;
SYSTEM_STATUS currentStatus=MOTOR_NOT_CONNECTED;
States state=IDLE;
uint8_t error=0;
bool newData=false,disp_updateRequired=false;
DriveType selectedDrive=DRIVE_TYPE_TSE;
DriveType motor_get_drive_type(){return selectedDrive;}
bool controller_is_menu_state(States s){return s==MENU1;}
int16_t motor_get_effective_set_rpm(){return motor_setRPM;}
int16_t motor_get_formatted_command_value(MotorCommandId,int8_t sign){return sign>0?100:-35;}
const auto& api=MOTOR_BACKEND_TSE.api;
void pump(int n=1){for(int i=0;i<n;++i){fakeNow+=25;if(simulatePeerTraffic)fakePeerAt=fakeNow;api.tick();}}
void fixture(){
 fakePeerActive=true;simulatePeerTraffic=false;fakePeerAt=fakeNow;
 Serial2=FakeUart{};selectedDrive=DRIVE_TYPE_TSE;state=IDLE;error=0;
 for(uint16_t r=0;r<=108;++r)Serial2.registers[r]=0;
 for(uint16_t r=0x100;r<=0x127;++r)Serial2.registers[r]=0;
 for(uint16_t r=0x1000;r<=0x1016;++r)Serial2.registers[r]=0;
 Serial2.registers[1]=6;Serial2.registers[2]=0x1F5C;
 Serial2.registers[62]=4;Serial2.registers[71]=1;Serial2.registers[72]=96;
 Serial2.registers[4]=1;Serial2.registers[22]=1;Serial2.registers[23]=5000;
 Serial2.registers[24]=static_cast<uint16_t>(-1000);
 Serial2.registers[0x100]=1;Serial2.registers[0x101]=2;Serial2.registers[0x102]=3;Serial2.registers[0x103]=4;
 Serial2.registers[0x110]=2;Serial2.registers[0x1009]=1;
 comm_init();comm_set_profile(COMM_PROFILE_TSE,true);api.init();
}
void complete(){
 for(int i=0;i<300;++i){pump();const auto& d=motor_tse_diagnostics();
  if(std::strcmp(d.phase,"complete")==0&&d.monitorValid&&d.alarmValid)return;
 }assert(false&&"initialisation failed");
}
std::vector<uint8_t> frame(std::vector<uint8_t> b){auto c=modbus_crc16(b.data(),b.size());b.push_back(c);b.push_back(c>>8);return b;}
int main(){
 fixture();complete();
 assert(commStatus==COMM_CONNECTED&&error==0);
 assert(motor_tse_diagnostics().startup.speedCommand==-1000);
 assert(motor_tse_diagnostics().startup.beforeAnyBackendWrite);
 CommDebugTransaction t{};
 // A synchronous raw read can safely replace only an idle telemetry read.
 for(int i=0;i<20&&!motor_tse_diagnostics().pending;++i)pump();
 assert(motor_tse_diagnostics().pending);
 auto oldCancelled=motor_tse_diagnostics().cancelledPollReads;
 auto oldTimeouts=motor_tse_diagnostics().timeouts;
 assert(comm_debug_raw_read_uart(9600,SERIAL_8N2,1,1,2,&t));
 assert(!t.blocked&&t.response_crc_ok&&t.response[4]==6);
 assert(motor_tse_diagnostics().cancelledPollReads==oldCancelled+1);
 pump(60);assert(motor_tse_diagnostics().timeouts==oldTimeouts&&error==0);
 for(int i=0;i<30;++i){assert(comm_debug_raw_read(COMM_PROFILE_TSE,1,22,4,&t));pump(3);}
 pump(30);assert(motor_tse_diagnostics().timeouts==oldTimeouts&&error==0);
 assert(Serial2.baud==9600&&Serial2.config==SERIAL_8N2);
 // Reads are refused without UART side effects while a run is requested.
 motor_setRPM=100;auto sent=Serial2.sent.size();
 assert(!comm_debug_raw_read_uart(19200,SERIAL_8N1,1,1,1,&t));
 assert(t.blocked&&!t.timed_out&&!t.response_crc_checked&&t.request_len==0&&Serial2.sent.size()==sent);
 assert(Serial2.baud==9600&&Serial2.config==SERIAL_8N2);motor_setRPM=0;
 assert(!comm_debug_raw_write_single(COMM_PROFILE_TSE,1,152,0,&t));
 assert(t.blocked&&!t.timed_out&&Serial2.sent.size()==sent);
 // UART diagnostics must not erase a previously reported drive alarm.
 error=3;errorCode=42;assert(comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));
 assert(error==3&&errorCode==42);error=0;errorCode=0;
 // An invalid frame / diagnostic failure cannot become a backend fault.
 Serial2.useOverride=true;
 for(auto b:std::vector<std::vector<uint8_t>>{
     frame({2,3,2,0,6}),frame({1,6,0,1,0,6}),frame({1,3,2,0,6,0}),
     std::vector<uint8_t>{1,3,2,0,6,0,0},frame({1,3,4,0,6,0,7})}){
  Serial2.overrideReply=b;assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));
 }
 Serial2.overrideReply={};assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));assert(t.timed_out&&!t.blocked&&!t.response_crc_checked);
 Serial2.overrideReply=frame({1,0x83,2});assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));assert(t.exception_code==2&&t.response_crc_ok);
 Serial2.overrideReply=frame({1,0x83,2});Serial2.overrideReply.back()^=1;
 assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));assert(t.exception_code==0&&!t.response_crc_ok);
 Serial2.overrideReply=frame({2,0x83,2});assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));assert(t.exception_code==0);
 Serial2.useOverride=false;pump(60);assert(error==0&&motor_tse_diagnostics().timeouts==oldTimeouts);
 // Bad UART settings time out locally and the original profile is restored.
 assert(!comm_debug_raw_read_uart(19200,SERIAL_8N1,1,1,1,&t));assert(t.timed_out&&!t.blocked&&!t.response_crc_checked);
 assert(Serial2.baud==9600&&Serial2.config==SERIAL_8N2);pump(20);assert(error==0);
 // For another backend, strict FC06 validation still checks address/value/size.
 selectedDrive=DRIVE_TYPE_JMC;Serial2.useOverride=true;
 for(auto b:std::vector<std::vector<uint8_t>>{
     frame({1,6,0,25,0,0}),frame({1,6,0,152,0,1}),frame({2,6,0,152,0,0}),frame({1,6,0,152,0,0,0})}){
  Serial2.overrideReply=b;assert(!comm_debug_raw_write_single_uart(9600,SERIAL_8N2,1,152,0,&t));
 }
 Serial2.overrideReply=frame({1,6,0,152,0,0});assert(comm_debug_raw_write_single_uart(9600,SERIAL_8N2,1,152,0,&t));
 Serial2.useOverride=false;selectedDrive=DRIVE_TYPE_TSE;
#if defined(SG98_TSE_KFM_COMMISSIONING) && SG98_TSE_KFM_COMMISSIONING
 sent=Serial2.sent.size();auto rxBefore=Serial2.rx;
 assert(!comm_debug_raw_read(COMM_PROFILE_TSE,0,1,1,&t));assert(t.blocked&&!t.timed_out);
 assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,0xFFFF,1,&t));assert(t.blocked&&!t.timed_out);
 assert(!comm_debug_raw_write_single(COMM_PROFILE_TSE,1,0x10F,1,&t));assert(t.blocked&&!t.timed_out);
 assert(Serial2.sent.size()==sent&&Serial2.rx==rxBefore);
 fakeNow+=1000;assert(!writeSingleRegister(0x10F,1));assert(Serial2.rx==rxBefore);
 assert(!writeMultipleRegisters(24,2,1000,0));assert(Serial2.rx==rxBefore);
 assert(!sew_movilink_send_pd3(0,6,1000,1000));assert(Serial2.rx==rxBefore);
 assert(Serial2.sent.size()==sent);
#endif
 // Validate the normal receive path too: malformed counts never become
 // zero-filled successful reads for the probe/preflight caller.
 fixture();complete();Serial2.useOverride=true;
 for(auto b : std::vector<std::vector<uint8_t>>{
     frame({1,3,255,0,0,0,0,0,0,0,0}), frame({1,3,3,0,0,0,0,0,0,0,0}),
     frame({2,3,8,0,1,0,2,0,3,0,4}),frame({1,3,8,0,1,0,2,0,3,0,4,0,5})}) {
  Serial2.overrideReply=b;fakeNow+=1000;assert(readRegister(22,4));fakeNow+=1000;
  assert(receive()==0&&lastRequestType==104);
 }
 Serial2.overrideReply=frame({1,3,8,0,1,0,2,0,3,0,4});fakeNow+=1000;
 assert(readRegister(22,4));fakeNow+=1000;assert(receive()==22&&lastRead[3]==4);
 fakeNow+=1000;assert(!readRegister(22,0)&&!readRegister(22,14));
 for(auto b : std::vector<std::vector<uint8_t>>{
     frame({2,6,0,152,0,0}),frame({1,6,0,25,0,0}),frame({1,6,0,152,0,1}),frame({1,3,0,24,0,0})}) {
  Serial2.overrideReply=b;fakeNow+=1000;assert(writeSingleRegister(152,0));fakeNow+=1000;assert(receive()==0);
 }
 Serial2.useOverride=false;
 // Thousands of deterministic valid/malformed frames exercise the production
 // parser under sanitizers, including malicious byte counts and exceptions.
 Serial2.useOverride=true;
 for(unsigned i=0;i<2048;++i) {
  unsigned kind=i%8;std::vector<uint8_t> bytes={1,3,4,uint8_t(i>>8),uint8_t(i),0,6};
  if(kind==1)bytes[2]=255;
  if(kind==2)bytes[0]=2;
  if(kind==3)bytes[2]=2;
  if(kind==4)bytes[1]=6;
  if(kind==5){bytes.resize(29,0xA5);bytes[2]=26;}
  if(kind==6)bytes={1,0x83,2};
  Serial2.overrideReply=frame(bytes);if(kind==7)Serial2.overrideReply.back()^=1;
  fakeNow+=1000;assert(readRegister(22,2));fakeNow+=1000;auto result=receive();
  if(result!=(kind==0?22:kind==6?0x603F:0)) std::fprintf(stderr,"fuzz i=%u kind=%u result=%d reqtype=%u len=%zu\n",i,kind,result,lastRequestType,Serial2.overrideReply.size());
  assert(result==(kind==0?22:kind==6?0x603F:0));
  assert(lastRequestType==102);
  assert(comm_last_response_was_exception()==(kind==6));
 }
 Serial2.useOverride=false;
#if defined(SG98_TSE_KFM_COMMISSIONING) && SG98_TSE_KFM_COMMISSIONING
 fakeNow+=1000;assert(writeSingleRegister(0x98,0));
 assert(!writeSingleRegister(0x10F,1)&&!writeMultipleRegisters(24,2,1000,0));
 fakeNow+=1000;assert(receive()==0x98); // original acknowledgement survives blocked commands
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
 // A truncated source/preset response must not permit SON=1.
 fixture();complete();Serial2.useOverride=true;Serial2.overrideReply=frame({1,3,2,0,1});
 auto noEnableStart=Serial2.sent.size();assert(motor_tse_zero_probe_start());pump();
 assert(motor_tse_zero_probe_view().phase==kfm_zero_probe::Phase::Failed);
 for(size_t i=noEnableStart;i<Serial2.sent.size();++i)assert(Serial2.sent[i][1]==3);
 Serial2.useOverride=false;
 // The previous raw-write parser cases simulated a DIFFERENT active backend.
 // Selecting TSE again creates a fresh backend lifecycle, as in the app.
 fixture();complete();
 assert(motor_tse_zero_probe_available());
 for(uint16_t coldSon : {0,1}) {
  pump(30);assert(error==0);
  assert(motor_tse_zero_probe_start());
  for(int i=0;i<50 && !motor_tse_zero_probe_view().holdVerified;++i)pump();
  assert(motor_tse_zero_probe_view().holdVerified&&Serial2.registers[0x10F]==1);
  assert(Serial2.registers[0x110]==2&&Serial2.registers[25]==0);
  auto heldSent=Serial2.sent.size();
  assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,1,1,&t));assert(t.blocked&&!t.timed_out);
  assert(!motor_tse_store_zero_default());
  fakeNow+=1000;assert(!writeSingleRegister(0x10F,1));
  assert(Serial2.sent.size()==heldSent); // no public write window, even while holding
  Serial2.linkUp=false;
  for(int i=0;i<30&&!motor_tse_zero_probe_view().communicationGapObserved;++i)pump();
  assert(motor_tse_zero_probe_view().communicationGapObserved);
  const auto gapSent=Serial2.sent.size();
  for(size_t i=heldSent;i<gapSent;++i)assert(Serial2.sent[i][1]==3); // observe power loss without rewriting flags
  Serial2.linkUp=true;Serial2.registers[0x10F]=coldSon;Serial2.registers[24]=static_cast<uint16_t>(-1000);
  for(int i=0;i<50&&motor_tse_zero_probe_view().phase!=kfm_zero_probe::Phase::Complete;++i)pump();
  const auto& v=motor_tse_zero_probe_view();
  assert(v.phase==kfm_zero_probe::Phase::Complete&&v.restoreAcknowledged);
  assert(v.after.forced[0]==coldSon&&static_cast<int16_t>(v.after.source[2])==-1000);
  assert(Serial2.registers[0x10F]==0&&Serial2.registers[0x110]==2&&Serial2.registers[24]==0);
  pump(50);assert(error==0&&motor_currentRPM==0&&!motor_tse_diagnostics().motionArmed);
 }
 assert(motor_tse_zero_probe_start());
 for(int i=0;i<50&&!motor_tse_zero_probe_view().holdVerified;++i)pump();
 assert(api.off());assert(Serial2.registers[0x10F]==0);
 assert(motor_tse_zero_probe_view().phase==kfm_zero_probe::Phase::Cancelled);
#else
 assert(!motor_tse_zero_probe_available()&&!motor_tse_zero_probe_start());
#endif

#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
 assert(motor_tse_pa25_probe_available()&&!motor_tse_zero_probe_available());
 // Explicit recovery before the first startup snapshot must count as writes.
 fixture();Serial2.registers[25]=1;
 assert(motor_tse_pa25_probe_recover());
 assert(!motor_tse_diagnostics().startup.beforeAnyBackendWrite);
 assert(motor_tse_pa25_probe_view().restoreReadbackValid);

 // Reproduce loss of RAM state with a retained servo-side marker.
 fixture();Serial2.registers[25]=1;
 for(int i=0;i<140;++i)pump();
 bool recovered=false;
 for(int i=0;i<40&&!recovered;++i){recovered=motor_tse_pa25_probe_recover();if(!recovered)pump();}
 assert(recovered&&Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
 assert(motor_tse_pa25_probe_view().restoreReadbackValid);
 complete();assert(error==0);
 fixture();complete();Serial2.registers[1]=7;auto recoveryStart=Serial2.sent.size();
 for(int i=0;i<20;++i){assert(!motor_tse_pa25_probe_recover());pump();}
 for(size_t i=recoveryStart;i<Serial2.sent.size();++i)if(Serial2.sent[i][1]==6)assert(Serial2.sent[i][2]!=0||Serial2.sent[i][3]!=0x99);
 fixture();complete();

 for(uint16_t returned:{0,1}){
  fixture();complete();
  fakeNow+=1000;assert(!writeSingleRegister(0x99,1));
  assert(motor_tse_pa25_probe_start());
  for(int i=0;i<60&&!motor_tse_pa25_probe_view().holdVerified;++i)pump();
  assert(motor_tse_pa25_probe_view().holdVerified&&Serial2.registers[25]==1&&Serial2.registers[0x10F]==0);
  auto count=Serial2.sent.size();fakeNow+=1000;
  assert(!writeSingleRegister(0x99,1)&&!writeSingleRegister(0x99,0)&&!writeSingleRegister(0x10F,1));
  assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,25,1,&t)&&t.blocked);
  assert(Serial2.sent.size()==count);
  Serial2.linkUp=false;
  for(int i=0;i<40&&!motor_tse_pa25_probe_view().communicationGapObserved;++i)pump();
  assert(motor_tse_pa25_probe_view().communicationGapObserved);
  for(size_t i=count;i<Serial2.sent.size();++i)assert(Serial2.sent[i][1]==3);
  Serial2.linkUp=true;Serial2.registers[25]=returned;Serial2.registers[24]=static_cast<uint16_t>(-1000);
  for(int i=0;i<120&&motor_tse_pa25_probe_view().phase!=kfm_zero_probe::Phase::Complete;++i)pump();
  const auto& v=motor_tse_pa25_probe_view();
  assert(v.phase==kfm_zero_probe::Phase::Complete&&v.after.source[3]==returned&&v.restoreAcknowledged);
  assert(Serial2.registers[25]==0&&Serial2.registers[0x10F]==0&&Serial2.registers[0x110]==2);
  for(const auto& f:Serial2.sent)if(f.size()==8&&f[1]==6){
   unsigned reg=(f[2]<<8)|f[3],value=(f[4]<<8)|f[5];
   assert((reg==0x99&&value<=1)||(reg==0x98&&value==0)||(reg==0x10F&&value==0)||(reg==0x110&&value==2));
  }
 }
 fixture();complete();Serial2.useOverride=true;Serial2.overrideReply=frame({1,3,2,0,1});
 auto before=Serial2.sent.size();assert(motor_tse_pa25_probe_start());pump();
 assert(motor_tse_pa25_probe_view().phase==kfm_zero_probe::Phase::Failed);
 for(size_t i=before;i<Serial2.sent.size();++i)assert(Serial2.sent[i][1]==3);
 Serial2.useOverride=false;
 fixture();complete();assert(motor_tse_pa25_probe_start());
 for(int i=0;i<60&&!motor_tse_pa25_probe_view().holdVerified;++i)pump();
 assert(api.off()&&Serial2.registers[25]==0&&motor_tse_pa25_probe_view().phase==kfm_zero_probe::Phase::Cancelled);
#else
 assert(!motor_tse_pa25_probe_available()&&!motor_tse_pa25_probe_start());
#endif

#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE && (!defined(SG98_TSE_KFM_TOUCH_TEST) || !SG98_TSE_KFM_TOUCH_TEST)
 fixture();complete();assert(motor_tse_pa25_pulse_available()&&!motor_tse_pa25_probe_available());
 assert(!motor_tse_arm());
 assert(motor_tse_pa25_pulse_start());
 for(int i=0;i<70&&motor_tse_pa25_pulse_view().phase!=kfm_pa25_pulse::Phase::Running;++i)pump();
 assert(motor_tse_pa25_pulse_view().phase==kfm_pa25_pulse::Phase::Running);
 assert(Serial2.registers[25]==static_cast<uint16_t>(-100)&&Serial2.registers[0x10F]==1&&Serial2.registers[0x110]==2);
 auto pulseSent=Serial2.sent.size();fakeNow+=10;
 assert(!writeSingleRegister(0x99,static_cast<uint16_t>(-100))&&!writeSingleRegister(0x10F,1));
 assert(!comm_debug_raw_read(COMM_PROFILE_TSE,1,25,1,&t)&&t.blocked&&Serial2.sent.size()==pulseSent);
 Serial2.registers[0x1000]=static_cast<uint16_t>(-95);Serial2.simulateRpmDecay=true;
 for(int i=0;i<160&&(motor_tse_pa25_pulse_view().phase==kfm_pa25_pulse::Phase::Running||motor_tse_pa25_pulse_view().phase==kfm_pa25_pulse::Phase::Stopping);++i)pump();
 const auto& pulse=motor_tse_pa25_pulse_view();
 assert(pulse.phase==kfm_pa25_pulse::Phase::Complete&&pulse.motionObserved&&pulse.restoreReadbackValid);
 assert(Serial2.registers[25]==0&&Serial2.registers[0x10F]==0&&Serial2.registers[0x110]==2);
 for(const auto& f:Serial2.sent)if(f.size()==8&&f[1]==6){unsigned reg=(f[2]<<8)|f[3],value=(f[4]<<8)|f[5];assert((reg==0x99&&(value==0||value==65436))||(reg==0x98&&value==0)||(reg==0x10F&&value<=1)||(reg==0x110&&value==2));}
 fixture();Serial2.registers[25]=static_cast<uint16_t>(-100);Serial2.registers[0x10F]=1;
 assert(motor_tse_pa25_pulse_recover());assert(Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
 assert(!motor_tse_diagnostics().startup.beforeAnyBackendWrite);
 fixture();complete();Serial2.useOverride=true;Serial2.overrideReply=frame({1,3,2,0,1});auto noPulseWrite=Serial2.sent.size();assert(motor_tse_pa25_pulse_start());pump();
 assert(motor_tse_pa25_pulse_view().phase==kfm_pa25_pulse::Phase::Failed);
 for(size_t i=noPulseWrite;i<Serial2.sent.size();++i)assert(Serial2.sent[i][1]==3);
 Serial2.useOverride=false;
 fixture();complete();assert(motor_tse_pa25_pulse_start());
 for(int i=0;i<70&&motor_tse_pa25_pulse_view().phase!=kfm_pa25_pulse::Phase::Running;++i)pump();
 assert(api.off());assert(Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
#else
#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 assert(motor_tse_pa25_pulse_available()&&!motor_tse_pa25_pulse_start());
#else
 assert(!motor_tse_pa25_pulse_available()&&!motor_tse_pa25_pulse_start());
#endif
#endif

#if defined(SG98_TSE_KFM_TOUCH_TEST) && SG98_TSE_KFM_TOUCH_TEST
 fixture();simulatePeerTraffic=true;complete();assert(kfm_pa25_pulse::kRunMs==5000);
 assert(motor_tse_touch_test_view().available&&!motor_tse_touch_test_request_start());
 assert(motor_tse_touch_test_arm());assert(currentStatus==MOTOR_READY);
 assert(!motor_tse_pa25_pulse_start()); // normal USB pulse cannot bypass the GUI request
 assert(motor_tse_touch_test_request_start());state=GRINDING;motor_setRPM=1100;
 assert(!motor_tse_touch_test_request_start());
 for(int i=0;i<80&&motor_tse_pa25_pulse_view().phase!=kfm_pa25_pulse::Phase::Running;++i)pump();
 assert(motor_tse_pa25_pulse_view().phase==kfm_pa25_pulse::Phase::Running);
 assert(Serial2.registers[25]==65436&&motor_setRPM==100&&state==GRINDING);
 assert(motor_tse_touch_test_request_stop());assert(state==IDLE&&motor_setRPM==0&&Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
 assert(!motor_tse_touch_test_view().armed&&!motor_tse_touch_test_request_start());
 // Max-duration stop without a GUI Stop; no auto rearm or second start.
 fixture();simulatePeerTraffic=true;complete();assert(motor_tse_touch_test_arm());assert(motor_tse_touch_test_request_start());state=GRINDING;motor_setRPM=1100;
 for(int i=0;i<350&&motor_tse_touch_test_view().runActive;++i)pump();
 assert(!motor_tse_touch_test_view().runActive&&state==IDLE&&Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
 assert(motor_tse_pa25_pulse_view().phase==kfm_pa25_pulse::Phase::Complete&&!motor_tse_touch_test_request_start());
 // A disappeared remote stops a run and never re-arms on traffic return.
 fixture();simulatePeerTraffic=true;complete();assert(motor_tse_touch_test_arm());assert(motor_tse_touch_test_request_start());state=GRINDING;
 for(int i=0;i<80&&motor_tse_pa25_pulse_view().phase!=kfm_pa25_pulse::Phase::Running;++i)pump();
 simulatePeerTraffic=false;fakePeerAt=fakeNow-3100;pump();
 assert(!motor_tse_touch_test_view().armed&&!motor_tse_touch_test_view().runActive&&state==IDLE&&Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
 simulatePeerTraffic=true;pump();assert(!motor_tse_touch_test_request_start());
 fixture();simulatePeerTraffic=true;complete();assert(motor_tse_touch_test_arm());fakeNow+=31000;pump();assert(!motor_tse_touch_test_view().armed&&!motor_tse_touch_test_request_start());
 fixture();complete();fakePeerAt=0;assert(!motor_tse_touch_test_arm());
 fixture();simulatePeerTraffic=true;complete();fakePeerActive=false;assert(!motor_tse_touch_test_arm());
 fixture();simulatePeerTraffic=true;complete();motor_setRPM=1100;assert(!motor_tse_touch_test_arm());motor_setRPM=0;
 fixture();simulatePeerTraffic=true;complete();assert(motor_tse_touch_test_arm());assert(motor_tse_touch_test_disarm()&&!motor_tse_touch_test_view().armed&&!motor_tse_touch_test_request_start());
#else
 assert(!motor_tse_touch_test_view().available&&!motor_tse_touch_test_arm()&&!motor_tse_touch_test_request_start());
#endif
 std::puts("Production KFM UART integration tests passed");
}
