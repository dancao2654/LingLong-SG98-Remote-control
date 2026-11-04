#include "TSE/kfm_commissioning_policy.h"
#define main previous_transport_main
#include "kfm_transport_harness.cpp"
#undef main
void prepareCandidate() {
 fixture();simulatePeerTraffic=true;complete();pump(50);
 assert(motor_tse_diagnostics().stopReadbackVerified);
 assert(motor_tse_arm());
}
int main() {
 prepareCandidate();
 // Real comm.cpp enforces a 60ms send gap but can accept the previous
 // response at 50ms. A fast loop must retry the unsent command without
 // throwing away the completed zero-enable readbacks.
 state=GRINDING;motor_setRPM=100;
 for(int i=0;i<5000;++i){++fakeNow;if(simulatePeerTraffic)fakePeerAt=fakeNow;api.tick();}
 assert(Serial2.registers[25]==static_cast<uint16_t>(-100));
 assert(api.off());
 prepareCandidate();
 CommDebugTransaction t{};
 assert(!comm_debug_raw_write_single(COMM_PROFILE_TSE,1,0x99,100,&t)&&t.blocked);
 assert(!comm_debug_raw_write_single(COMM_PROFILE_TSE,1,0x10F,1,&t)&&t.blocked);
 state=GRINDING;motor_setRPM=kfm_commissioning::maxRpm+100;pump(120);
 assert(Serial2.registers[25]==static_cast<uint16_t>(-kfm_commissioning::maxRpm));
 assert(Serial2.registers[24]==static_cast<uint16_t>(-1000));
 assert(Serial2.registers[0x10F]==1&&Serial2.registers[0x110]==2);
 assert(motor_tse_diagnostics().motionArmed&&error==0);
 bool sonSeen=false,sourceVerified=false,sonVerified=false;
 for(const auto& f:Serial2.sent) {
  if(f.size()!=8) continue;
  const unsigned reg=(unsigned(f[2])<<8)|f[3],value=(unsigned(f[4])<<8)|f[5];
  if(f[1]==6&&reg==0x10F&&value==1)sonSeen=true;
  if(sonSeen&&f[1]==3&&reg==22)sourceVerified=true;
  if(sonSeen&&f[1]==3&&reg==0x10F)sonVerified=true;
  if(f[1]==6&&reg==0x99&&value!=0)assert(sonSeen&&sourceVerified&&sonVerified);
  if(f[1]==6)assert(reg==0x99||reg==0x10F||reg==0x110);
 }
 motor_setRPM=200;pump(70);assert(Serial2.registers[25]==static_cast<uint16_t>(-200));
 simulatePeerTraffic=false;pump(170);
 assert(!motor_tse_diagnostics().motionArmed&&state==IDLE&&motor_setRPM==0);
 assert(Serial2.registers[25]==0&&Serial2.registers[0x10F]==0);
 simulatePeerTraffic=true;pump(60);assert(!motor_tse_diagnostics().motionArmed);
 assert(motor_tse_diagnostics().stopReadbackVerified);
 prepareCandidate();state=GRINDING;motor_setRPM=100;pump(70);
 Serial2.registers[25]=200;pump(90); // external preset drift latches stop
 assert(!motor_tse_diagnostics().motionArmed&&motor_setRPM==0&&Serial2.registers[25]==0);
 // A successful stop ACK cannot conceal a motor still coasting.
 prepareCandidate();state=GRINDING;motor_setRPM=100;pump(70);
 Serial2.registers[0x1000]=static_cast<uint16_t>(-80);pump(30);
 state=IDLE;motor_setRPM=0;pump(70);
 assert(!motor_tse_arm()&&!motor_tse_diagnostics().stopReadbackVerified);
 Serial2.registers[0x1000]=0;pump(70);
 assert(motor_tse_diagnostics().stopReadbackVerified&&motor_tse_arm());
 assert(api.off());
 std::puts("Candidate real UART/gate regression passed (no physical device)");
}
