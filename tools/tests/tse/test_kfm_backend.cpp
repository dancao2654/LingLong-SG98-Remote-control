// Reuse the simulated serial transport; test the production backend, not a copy.
#define main family_regression_main
#include "test_tse_backend.cpp"
#undef main
#include <cstring>

void kfmFixture() {
    fixture();
    registers[1] = 6; registers[2] = 0x1F5C;
    registers[4] = 1; registers[22] = 1; registers[23] = 5000;
    registers[24] = static_cast<uint16_t>(-1500); registers[25] = 0;
    registers[0x0100] = 1; registers[0x0110] = 2;
}
void kfmComplete() {
    for(int i = 0; i < 200; ++i) {
        pump();
        const auto& d = motor_tse_diagnostics();
        if(std::strcmp(d.phase, "complete") == 0 && d.monitorValid && d.alarmValid) return;
    }
    assert(false && "KFM setup never completed");
}
int main() {
    kfmFixture();
    MotorProbeReport report = {};
    assert(api.probe(&report) && report.tse_signature);
    assert(writes.empty());
    for(const auto pair : std::vector<std::pair<uint16_t,uint16_t>>{{1,7},{2,0x1F5D},{62,5},{71,2},{72,192},{73,1}}) {
        kfmFixture(); registers[pair.first] = pair.second;
        assert(!api.probe(&report)); pump(100); assert(writes.empty());
    }
    kfmFixture(); kfmComplete();
    assert(currentStatus == MOTOR_NOT_READY && error == 0);
    assert(registers[23] == 5000 && registers[74] == 0);
    assert(registers[0x0100] == 1 && registers[0x0110] == 2);
    assert(registers[24] == 0);
    const auto startup = motor_tse_diagnostics().startup;
    assert(startup.captured && startup.beforeAnyBackendWrite && startup.validMask == 15);
    assert(startup.speedCommand == -1500 && startup.zeroPreset == 0);
    assert(startup.speedSource == 1 && startup.speedLimit == 5000);
    assert(startup.softwareEnable == 0 && startup.forcedInputs[0] == 0 && startup.forcedInputs[1] == 2);
    assert(startup.actualRpm == 0);
    pump(100);
    assert(motor_tse_diagnostics().startup.speedCommand == -1500); // frozen before zeroing

    for(const auto& w : writes) assert((w.reg == 0x98 && w.value == 0) || (w.reg == 0x10F && w.value == 0) || (w.reg == 0x110 && w.value == 2));
    kfmFixture();
    corruptCount = true; pump(30); // failed audit causes a conservative stop attempt
    corruptCount = false; kfmComplete();
    assert(motor_tse_diagnostics().startup.captured);
    assert(!motor_tse_diagnostics().startup.beforeAnyBackendWrite);
    for(const auto pair : std::vector<std::pair<uint16_t,uint16_t>>{{0x100,0},{0x101,7},{0x102,0},{0x103,0},{0x10F,1},{0x110,0},{0x111,1},{0x112,1},{0x113,1},{0x11E,1},{0x11F,1},{4,0},{22,0},{25,100},{23,499}}) {
        kfmFixture(); registers[pair.first] = pair.second;
        pump(100);
        const auto& d = motor_tse_diagnostics();
        assert(currentStatus == MOTOR_NOT_READY && error == 4);
        assert(d.rejectedRegister == pair.first);
        assert(writes.empty()); // No setup changes before factory preflight succeeds.
    }
#if !defined(SG98_TSE_KFM_ALLOW_MOTION) || !SG98_TSE_KFM_ALLOW_MOTION
    kfmFixture(); kfmComplete(); assert(!motor_tse_arm());
    writes.clear(); state = GRINDING; motor_setRPM = 1500; pump(100);
    assert(motor_setRPM == 0 && !wrote(0x10F, 1) && !wrote(0x110, 0));
    for(const auto& w : writes) assert(w.value == 0 || (w.reg == 0x110 && w.value == 2));
#else
    kfmFixture(); registers[24] = 0; kfmComplete();
    bool armed = false;
    for(int i=0;i<30 && !armed;++i) { armed = motor_tse_arm(); if(!armed) pump(); }
    assert(armed && motor_tse_diagnostics().motionArmed);
    writes.clear(); runAt(1000);
    assert(wrote(0x98, -500));
    assert(indexOf(0x110,0) < indexOf(0x98,-500));
    assert(indexOf(0x98,-500) < indexOf(0x10F,1));
    assert(!wrote(0xB5,1) && !wrote(0xA2,100));
    writes.clear(); assert(api.off());
    assert(indexOf(0x98,0) < indexOf(0x10F,0));
    assert(indexOf(0x10F,0) < indexOf(0x110,2));
    assert(!motor_tse_diagnostics().motionArmed);
    kfmFixture(); registers[24] = 0; kfmComplete();
    for(int i=0;i<30 && !motor_tse_arm();++i) pump();
    direction = -1; writes.clear(); runAt(100);
    assert(wrote(0x98,100)); // factory reverse is positive, normal is negative
    assert(api.off());
    kfmFixture(); registers[24] = 0; kfmComplete();
    for(int i=0;i<30 && !motor_tse_arm();++i) pump();
    runAt(400); linkUp=false; pump(40);
    assert(!motor_tse_diagnostics().motionArmed && motor_setRPM==0);
    linkUp=true; kfmComplete(); pump(30);
    assert(currentStatus==MOTOR_NOT_READY && motor_setRPM==0);
    kfmFixture(); registers[24] = 0; kfmComplete();
    for(int i=0;i<30 && !motor_tse_arm();++i) pump();
    runAt(400); registers[0x1013]=7; pump(40);
    assert(!motor_tse_diagnostics().motionArmed && motor_setRPM==0);
    assert(registers[24]==0 && registers[0x10F]==0 && registers[0x110]==2);
#endif
    kfmFixture(); kfmComplete();
    assert(!motor_tse_arm()); // nonzero power-on default cannot arm even after runtime zero
    bool stored = false;
    for(int i=0;i<30 && !stored;++i) { stored = motor_tse_store_zero_default(); if(!stored) pump(); }
    assert(stored && wrote(24,0));
    assert(motor_tse_diagnostics().startup.speedCommand == -1500); // storage does not rewrite history
    state=GRINDING;assert(!motor_tse_store_zero_default());state=IDLE;
    registers[0x1000]=10;pump(20);assert(!motor_tse_store_zero_default());
    kfmFixture(); kfmComplete();
    registers[0x1007]=static_cast<uint16_t>(-16);pump(30);
    assert(motor_tse_diagnostics().torqueRaw==-16&&motor_currentTorque==0);
    assert(motor_tse_diagnostics().torqueRawValid&&!motor_tse_diagnostics().torqueScalingVerified);
    exceptionReply = true; pump(12);
    assert(motor_tse_diagnostics().modbusExceptions==1);
    assert(motor_tse_diagnostics().timeouts==0);
    assert(std::strcmp(motor_tse_diagnostics().reason,"modbus_exception")==0);
    assert(motor_tse_diagnostics().actual==2);
    std::puts("KFM backend regression tests passed");
}
