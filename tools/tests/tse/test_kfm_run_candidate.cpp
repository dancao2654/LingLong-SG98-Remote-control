#include "TSE/kfm_commissioning_policy.h"
// Production backend with deterministic UART and remote-heartbeat simulation.
#define main legacy_family_main
#include "test_tse_backend.cpp"
#undef main
#include <cstring>
static bool peerActive = true;
static bool peerFresh = true;
bool espnow_control_is_active() { return peerActive; }
unsigned long espnow_control_last_command_rx_at() { return peerFresh ? now : now-4000; }
void candidateFixture() {
    fixture(); peerActive=true; peerFresh=true;
    registers[1]=6;registers[2]=0x1F5C;
    registers[4]=1;registers[22]=1;registers[23]=5000;
    registers[24]=static_cast<uint16_t>(-1000);registers[25]=0;
    registers[0x100]=1;registers[0x110]=2;
    pump(120);
    assert(std::strcmp(motor_tse_diagnostics().phase,"complete")==0);
    assert(motor_tse_diagnostics().stopReadbackVerified);
    assert(!motor_tse_diagnostics().motionArmed);
}
void armCandidate() {
    bool ok=false;
    for(int i=0;i<30&&!ok;++i) { ok=motor_tse_arm();if(!ok)pump(); }
    static int attempt=0; ++attempt; if(!ok) { const auto& d=motor_tse_diagnostics(); std::fprintf(stderr,"arm %d failed: reason=%s phase=%s stop=%d maskboot=%d before=%d source=%d son=%d sel=%d rpm=%d cmd=%d state=%d pending=%d arm=%d error=%d now=%u\n",attempt,d.reason,d.phase,d.stopReadbackVerified,d.startup.validMask,d.startup.beforeAnyBackendWrite,d.startup.zeroPreset,d.startup.forcedInputs[0],d.startup.forcedInputs[1],motor_currentRPM,motor_setRPM,state,d.pending,d.motionArmed,error,now); }
    assert(ok);
}
#ifndef SG98_KFM_FLOW_FIXTURE
int main() {
    // Missing servo must not discard the resting weighing screen.
    fixture(); state=IDLE_GBW; linkUp=false; pump(100);
    assert(state==IDLE_GBW && motor_setRPM==0 && !motor_tse_diagnostics().motionArmed);
    candidateFixture(); state=GRINDING_GBW; assert(!motor_tse_arm());
    state=PURGING; assert(!motor_tse_arm());
    state=IDLE_GBW; armCandidate();
    assert(state==IDLE_GBW && motor_tse_diagnostics().motionArmed);
    assert(api.off());
    candidateFixture();
    const auto boot=motor_tse_diagnostics().startup;
    // A read response can arrive before the transport permits another write.
    // Every completed zero-enable check encounters one unsent attempt. The
    // checked state must survive that deferral, otherwise it loops forever.
    simulateSpeedSendGap=true;armCandidate();runAt(100);pump(100);
    assert(deferredSpeedWrites>0);
    assert(wrote(0x99,-100) && registers[25]==static_cast<uint16_t>(-100));
    assert(api.off());candidateFixture();
    assert(boot.speedCommand==-1000 && boot.zeroPreset==0);
    assert(registers[24]==static_cast<uint16_t>(-1000));
    for(const auto& w:writes) assert(w.reg!=24 && w.reg!=0x98 && w.reg!=0xB5);
    state=GRINDING;motor_setRPM=1100;pump(30);
    assert(motor_setRPM==0&&!wrote(0x10F,1));
    state=IDLE;armCandidate();writes.clear();
    runAt(kfm_commissioning::maxRpm+100);pump(30);
    assert(wrote(0x99,-kfm_commissioning::maxRpm));
    assert(indexOf(0x10F,1)<indexOf(0x99,-kfm_commissioning::maxRpm));
    assert(!wrote(0x110,0)&&registers[0x110]==2);
    motor_setRPM=200;pump(30);assert(wrote(0x99,-200));
    // Reversal waits for measured zero, with speed=0 and SON=0 first.
    registers[0x1000]=static_cast<uint16_t>(-100);pump(20);
    writes.clear();direction=-1;pump(40);
    assert(wrote(0x99,0)&&wrote(0x10F,0)&&!wrote(0x99,200));
    registers[0x1000]=0;pump(50);
    assert(wrote(0x99,200));
    assert(indexOf(0x10F,0)<indexOf(0x99,200));
    // A purge-style zero pause must not lose the bounded session.
    motor_setRPM=0;pump(40);
    assert(registers[25]==0&&registers[0x10F]==0);
    assert(motor_tse_diagnostics().motionArmed);
    motor_setRPM=150;pump(40);assert(registers[25]==150);
    // Receiver loss stops and latches; reconnect cannot resume.
    peerActive=false;pump(40);
    assert(!motor_tse_diagnostics().motionArmed&&motor_setRPM==0&&state==IDLE);
    assert(registers[25]==0&&registers[0x10F]==0&&registers[0x110]==2);
    peerActive=true;pump(40);assert(!motor_tse_diagnostics().motionArmed);
    candidateFixture();armCandidate();runAt(200);pump(20);
    peerFresh=false;pump(40);assert(!motor_tse_diagnostics().motionArmed&&registers[25]==0);
    candidateFixture();armCandidate();runAt(200);pump(20);
    // No driver-side serial timeout is simulated: only the live MCU stop attempt.
    linkUp=false;pump(40);assert(!motor_tse_diagnostics().motionArmed&&motor_setRPM==0);
    linkUp=true;pump(120);assert(!motor_tse_diagnostics().motionArmed);
    for(uint16_t reg : {0x111,0x112,0x113,0x100F,0x11E,0x11F}) {
        candidateFixture();armCandidate();runAt(200);pump(30);
        registers[reg]=1;pump(120);
        assert(!motor_tse_diagnostics().motionArmed&&motor_setRPM==0&&registers[25]==0);
    }
    candidateFixture();armCandidate();runAt(200);pump(20);
    registers[0x1013]=7;pump(40);
    assert(!motor_tse_diagnostics().motionArmed&&registers[25]==0);
    candidateFixture();armCandidate();runAt(200);pump(20);
    registers[0x1000]=static_cast<uint16_t>(-kfm_commissioning::overspeedRpm-1);pump(40);
    assert(!motor_tse_diagnostics().motionArmed&&registers[25]==0);
    candidateFixture();armCandidate();runAt(200);pump(20);
    registers[0x1000]=100;pump(40);
    assert(!motor_tse_diagnostics().motionArmed&&registers[25]==0);
    candidateFixture();armCandidate();runAt(200);pump(1400);
    assert(!motor_tse_diagnostics().motionArmed&&registers[25]==0);
    assert(std::strcmp(motor_tse_diagnostics().reason,"candidate_run_timeout")==0);
    candidateFixture();armCandidate();pump(1400);
    assert(!motor_tse_diagnostics().motionArmed&&registers[25]==0);
    candidateFixture();state=IDLE_GBW;armCandidate();pump(1400);
    assert(state==IDLE_GBW && !motor_tse_diagnostics().motionArmed && registers[25]==0);
    assert(std::strcmp(motor_tse_diagnostics().reason,"candidate_arm_timeout")==0);
    // Re-init cannot turn a post-write read into a new pre-write boot audit.
    candidateFixture();api.init();pump(120);
    assert(motor_tse_diagnostics().startup.speedCommand==boot.speedCommand);
    assert(motor_tse_diagnostics().startup.completedAtMs==boot.completedAtMs);
    // Stop ACK is not stationary proof; rearm waits for actual coast-down.
    armCandidate();runAt(100);pump(20);registers[0x1000]=-80;pump(20);
    assert(api.off());state=IDLE;pump(40);
    assert(!motor_tse_diagnostics().stopReadbackVerified&&!motor_tse_arm());
    registers[0x1000]=0;pump(40);assert(motor_tse_diagnostics().stopReadbackVerified);
    armCandidate();assert(api.off());
    std::puts("PA25 run candidate regression passed (simulation only)");
}

#endif
