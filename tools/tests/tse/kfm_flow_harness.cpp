#include <cstdlib>
#include <sg98_intent_token.h>
#include <string>
#include <algorithm>
using std::min;
#define SG98_KFM_FLOW_FIXTURE 1
#include "test_kfm_run_candidate.cpp"
constexpr int rpm_scalar=25,absolute_min_rpm=0,absolute_max_rpm=3000,SIZEOFCALIBRATEARRAY=240;
struct GBW_WEIGHT { uint32_t time;int32_t weight; };
enum SCALE_STATUS {SCALE_DISCONNECTED,SCALE_CONNECTED,SCALE_CONNECTING,SCALE_LOST,INVALID_SCALE_STATUS};
extern volatile int32_t currentWeight;
struct FakeScale {bool autoZero=false;bool connected=true;bool isConnected(){return connected;}int tares=0;void tare(){++tares;if(autoZero)currentWeight=0;}void startTimer(){}void stopTimer(){}} scale;
struct Print {template<class T>void println(T){} template<class T>void print(T){}} Serial;
void delay(unsigned ms){now+=ms;}
void ledAction(int){}
bool motorOff(){return api.off();}
int16_t setRPM=100;
uint32_t setWeight=18000;
unsigned long lastActivity=0;
bool ready_purge=false;
int16_t calibrateArray[SIZEOFCALIBRATEARRAY]={};
void smart_purge_learning_reset_runtime(){}
template<class... T>void smart_purge_learning_note_sample(T...){}
template<class... T>uint16_t smart_purge_learning_threshold(T...){return 0;}
void gbw_learn(){} // Learning/real BLE hardware excluded from this harness.
enum DriveType {DRIVE_TYPE_UNKNOWN,DRIVE_TYPE_TSE};
DriveType motor_get_drive_type(){return DRIVE_TYPE_TSE;}
void motor_ensure_drive_detected(bool){}
bool shouldOpenDriveSelectorApi(){return false;}
void do_comm(){api.tick();}
void controller_note_activity(){lastActivity=now;}
sg98_intent::Ledger flowIntentLedger;
sg98_intent::Decision beginUserIntent(const char* token,char action){return flowIntentLedger.begin(token,action);}
void rotateUserIntentEpoch(){static uint64_t epoch=1;flowIntentLedger.reset(++epoch);}
void finishUserIntent(bool ok){flowIntentLedger.finish(ok);}
// Menu declarations precede the extracted production functions.
#include "production_flow_declarations.h"
#include "production_flows.h"
void flowTick(int n=1) {
 for(int i=0;i<n;++i) {
  if(state==GRINDING)checkPurge();
  else if(state==PURGING)do_purge();
  else if(state==GRINDING_GBW)do_gbw();
  else motor_setRPM=0;
  // Ideal response model; actual mechanical acceleration/latency is untested.
  registers[0x1000]=registers[0x10F]==1?registers[25]:0;
  pump();
 }
}
void setupFlow() {
 candidateFixture();now+=2000;pump(80);
 Menu1[SETMAX].value=3000;Menu1[SETMIN].value=0;
 Menu2[SETPURGETIME].value=2000;Menu2[SETPURGEOFFTIME].value=1000;
 Menu2[SETPURGERPM].value=200;Menu2[SETPURGEREVERSE].value=150;
 Menu3[GBW_RPM_SET].value=400;Menu3[GBW_SLOW_RPM].value=100;
 Menu3[GBW_SLOW_MG].value=2000;Menu3[GBW_SPEEDMOD].value=2500;
 Menu3[GBW_OFFSET].value=100;scaleStatus=SCALE_CONNECTED;
 armCandidate();
}
int main(int argc,char** argv) {
 assert(argc==2);const std::string mode=argv[1];const char* message=nullptr;
 setupFlow();
 if(mode=="standalone-intents") {
  Menu1[SETMAX].value=0;assert(purgeClampedRpm(2000)==0);Menu1[SETMAX].value=2000;
  motor_tse_disarm();pump(100);flowIntentLedger.seed(0x1234567812345678ULL);
  sg98_intent::Client client;char offer[25],a[25],b[25],c[25],d[25];flowIntentLedger.offer(offer);
  assert(!apiIntentAction(true,nullptr,&message));assert(!motor_tse_diagnostics().motionArmed);
  assert(client.next(offer,a));assert(apiIntentAction(true,a,&message));flowTick(100);
  assert(apiIntentAction(true,a,&message)); // retry acknowledges same run, no new action
  assert(client.next(offer,b));assert(apiIntentAction(false,b,&message));flowTick(100);
  assert(!apiIntentAction(true,a,&message));assert(!motor_tse_diagnostics().motionArmed);
  assert(client.next(offer,c));assert(apiIntentAction(true,c,&message));flowTick(80);
  Menu2[SETBUTTONPURGE].value=1;assert(client.next(offer,d));assert(apiIntentAction(false,d,&message));assert(state==PURGING);
  assert(apiIntentAction(false,d,&message));assert(state==PURGING); // repeat cannot abort its purge
  char e[25];assert(client.next(offer,e));assert(apiIntentAction(false,e,&message));flowTick(100);assert(state==IDLE);
  char rejected[25];assert(client.next(offer,rejected));state=MENU1;assert(!apiIntentAction(true,rejected,&message));
  state=IDLE;assert(!apiIntentAction(true,rejected,&message));assert(!motor_tse_diagnostics().motionArmed);
  char delayed[25],cancel[25];assert(client.next(offer,delayed));assert(client.next(offer,cancel));
  assert(apiIntentIdle(IDLE,cancel,&message));assert(!apiIntentAction(true,delayed,&message));
  // Unrecognized Stop still has a direct zero/disable path, never requires a token.
  char fresh[25];assert(client.next(offer,fresh));assert(apiIntentAction(true,fresh,&message));flowTick(80);
  assert(apiIntentAction(false,nullptr,&message));flowTick(100);assert(state==IDLE&&registers[25]==0);
  assert(!apiIntentAction(true,fresh,&message)); // emergency stop rotated epoch

 }else if(mode=="standalone") {
  motor_tse_disarm();pump(100);
  assert(!motor_tse_diagnostics().motionArmed);
  for(int i=0;i<100;++i)flowTick(); // cold boot never arms itself
  assert(!motor_tse_diagnostics().motionArmed && registers[25]==0);
  state=MENU1;assert(!apiActionStart(&message));assert(!motor_tse_diagnostics().motionArmed);
  state=IDLE;setRPM=2200;assert(apiActionStart(&message));flowTick(120);
  assert(wrote(0x99,-2000) && std::abs(motor_currentRPM)==2000);
  assert(apiActionStop(&message));flowTick(100);
  assert(motor_tse_diagnostics().stopReadbackVerified && !motor_tse_diagnostics().motionArmed);
  assert(apiActionStart(&message));flowTick(100); // new explicit Start, no USB arm
  peerFresh=false;flowTick(100);
  assert(!motor_tse_diagnostics().motionArmed && registers[25]==0 && motor_tse_stop_ack_required());
  peerFresh=true;flowTick(100);assert(!apiActionStart(&message)); // no restart on reconnect
  assert(apiActionIdle(IDLE,&message));assert(!motor_tse_stop_ack_required());assert(!motor_tse_diagnostics().motionArmed);
  Menu2[SETBUTTONPURGE].value=1;Menu2[SETPURGERPM].value=2000;Menu2[SETPURGEREVERSE].value=1700;
  assert(apiActionStart(&message));flowTick(100);assert(apiActionStop(&message));flowTick(600);
  assert(wrote(0x99,1700) && wrote(0x99,-2000));
  assert(state==IDLE && !motor_tse_diagnostics().motionArmed && motor_tse_diagnostics().stopReadbackVerified);
  Menu2[SETBUTTONPURGE].value=0;state=IDLE_GBW;currentWeight=68100;scale.autoZero=true;
  Menu3[GBW_RPM_SET].value=1500;Menu3[GBW_SLOW_RPM].value=500;
  assert(apiActionStart(&message));flowTick(140);currentWeight=5000;flowTick(30);
  assert(scale.tares>0 && wrote(0x99,-1500));currentWeight=17000;flowTick(30);
  assert(wrote(0x99,-500));currentWeight=19000;flowTick(200);
  assert(state==IDLE_GBW && !motor_tse_diagnostics().motionArmed);
  // Scale loss is a latched abort, even before/without a complete weighing shot.
  currentWeight=0;assert(apiActionStart(&message));flowTick(30);scale.connected=false;flowTick(100);
  assert(error==104 && motor_tse_stop_ack_required());scale.connected=true;flowTick(100);
  assert(!apiActionStart(&message));assert(apiActionIdle(IDLE,&message));assert(!motor_tse_stop_ack_required());
  state=IDLE;setRPM=1800;assert(apiActionStart(&message));flowTick(6500);
  assert(state==IDLE && !motor_tse_diagnostics().motionArmed && motor_tse_stop_ack_required());
  assert(!apiActionStart(&message));assert(apiActionIdle(IDLE,&message));assert(!motor_tse_stop_ack_required());
  error=6;assert(!apiActionStart(&message));assert(apiActionIdle(IDLE,&message));assert(!motor_tse_stop_ack_required());assert(error==0);
 }else if(mode=="manual") {
  motor_tse_disarm();assert(!apiActionStart(&message));pump(80);armCandidate();
  assert(apiActionStart(&message));flowTick(100);assert(wrote(0x99,-100));
  setRPM=1100;flowTick(80);assert(wrote(0x99,-500));
  assert(!apiActionStart(&message));assert(apiActionStop(&message));flowTick(100);
  assert(state==IDLE&&registers[25]==0&&registers[0x10F]==0);
  assert(motor_tse_diagnostics().stopReadbackVerified);
 }else if(mode=="manual-timeout") {
  assert(apiActionStart(&message));flowTick(1400);
  assert(state==IDLE&&registers[25]==0&&!motor_tse_diagnostics().motionArmed);
  assert(!apiActionStart(&message));
 }else if(mode=="purge") {
  Menu2[SETBUTTONPURGE].value=1;assert(apiActionStart(&message));flowTick(100);
  assert(apiActionStop(&message));assert(state==PURGING);flowTick(500);
  assert(wrote(0x99,150)&&wrote(0x99,-200));
  assert(state==IDLE&&registers[25]==0&&registers[0x10F]==0);
  pump(80);armCandidate();assert(apiActionStart(&message));flowTick(80);
  assert(apiActionStop(&message));flowTick(150);assert(apiActionStop(&message));flowTick(80);
  assert(state==IDLE&&registers[25]==0);
 }else if(mode=="radio") {
  assert(apiActionStart(&message));flowTick(100);peerFresh=false;flowTick(100);
  assert(state==IDLE&&!motor_tse_diagnostics().motionArmed&&registers[25]==0);
  peerFresh=true;flowTick(100);assert(!apiActionStart(&message));
 }else {
  if(mode=="gbw-tare-empty") {
   currentWeight=68100;scale.autoZero=true;
   state=IDLE_GBW;assert(apiActionStart(&message));flowTick(240);
   assert(scale.tares>0&&currentWeight==0);
   if(!(state==IDLE_GBW&&error==105&&registers[25]==0))std::fprintf(stderr,"tare-empty state=%d error=%d complete=%d started=%d rpmcmd=%d tares=%d\n",state,error,grindingComplete,gbw_started,motor_setRPM,scale.tares);
   assert(state==IDLE_GBW&&error==105&&registers[25]==0);
   return 0;
  }
  if(mode=="gbw-no-scale") {
   scaleStatus=SCALE_DISCONNECTED;scale.connected=false;
   state=IDLE_GBW;assert(apiActionStart(&message));flowTick(40);
   assert(state==IDLE_GBW&&error==106&&!wrote(0x99,-400));return 0;
  }
  state=IDLE_GBW;assert(apiActionStart(&message));flowTick(60);
  assert(gbw_started&&wrote(0x99,-400));
  if(mode=="gbw-target") {
   for(int i=0;i<40&&state==GRINDING_GBW;++i){currentWeight+=500;flowTick(4);}
   flowTick(120);assert(state==IDLE_GBW&&registers[25]==0);
  }else if(mode=="gbw-empty") {
   flowTick(140);assert(state==IDLE_GBW&&error==105&&registers[25]==0);
  }else if(mode=="gbw-disconnect") {
   currentWeight=1000;scaleStatus=SCALE_LOST;scale.connected=false;flowTick();
   assert(state==IDLE_GBW&&motor_setRPM==0&&error==104);
   flowTick(100);assert(registers[25]==0&&!motor_tse_diagnostics().motionArmed);
  }else if(mode=="gbw-link-loss") {
   linkUp=false;flowTick(60);
   assert(state==IDLE_GBW);
   assert(motor_setRPM==0&&!motor_tse_diagnostics().motionArmed);
   linkUp=true;flowTick(160);
   assert(registers[25]==0&&!motor_tse_diagnostics().motionArmed&&!apiActionStart(&message));
  }else if(mode=="gbw-overflow") {
   for(unsigned i=0;i<300;++i)_shot[i]={i,static_cast<int32_t>(i)};
   last_shot_updated=299;currentWeight=500;flowTick();
   assert(last_shot_updated==299&&shotHistoryTruncated);
   assert(_shot[0].time==1&&_shot[299].weight==500);
   for(int i=0;i<400;++i)append_shot_sample(500+i,500+i);
   assert(last_shot_updated==299&&_shot[299].weight==899);
  }else if(mode=="gbw-restart") {
   const auto firstStart=startOfShot;
   assert(apiActionStop(&message));const auto stoppedAt=now;
   for(int i=0;i<35&&!motor_tse_diagnostics().stopReadbackVerified;++i)flowTick();
   assert(now-stoppedAt<1000&&motor_tse_diagnostics().stopReadbackVerified);
   state=IDLE;armCandidate();state=IDLE_GBW;
   assert(apiActionStart(&message));do_gbw();
   assert(startOfShot>firstStart&&last_shot_updated==0&&!grindingComplete);
   assert(gbw_predict()>4500); // Fresh prediction window on a rapid new shot.
   error=105;gbw_prepare_for_grinding();assert(error==0);
   error=3;gbw_prepare_for_grinding();assert(error==3); // Drive errors stay intact.
  }else assert(false);
 }
 std::printf("Flow passed: %s\n",mode.c_str());
}
