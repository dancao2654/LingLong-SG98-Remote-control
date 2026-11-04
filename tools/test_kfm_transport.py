"""Run real Modbus, probe, TSE and MOVILINK code against a simulated UART.

No serial port is opened. Simulation verifies protocol and state handling; it
cannot prove the attached KFM's EEPROM behaviour or its physical stopping.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

ARDUINO_STUB = r'''

#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>
#include <algorithm>
using std::size_t;
using std::min;
using std::max;
extern unsigned long fakeNow;
inline unsigned long millis() { return fakeNow; }
inline void delay(unsigned long ms) { fakeNow += ms; }
inline void delayMicroseconds(unsigned long) { }
constexpr int OUTPUT = 1, HIGH = 1, LOW = 0, HEX = 16;
constexpr uint32_t SERIAL_8N1 = 1, SERIAL_8N2 = 2, SERIAL_8E1 = 3, SERIAL_8O1 = 4;
inline void pinMode(int, int) { }
inline void digitalWrite(int, int) { }
struct FakePrint {
    template<typename... T> void print(T...) { }
    template<typename... T> void println(T...) { }
    template<typename... T> void printf(T...) { }
};
extern FakePrint Serial;
uint16_t modbus_crc16(uint8_t*, int);
struct FakeUart {
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint8_t> rx, overrideReply;
    std::map<uint16_t, uint16_t> registers;
    uint32_t baud = 0, config = 0;
    bool useOverride = false, linkUp = true, simulateRpmDecay = false;
    unsigned stopReadCountdown = 0;
    void begin(uint32_t b, uint32_t c, int, int) { baud=b; config=c; }
    void end() { }
    void flush() { }
    int available() { return static_cast<int>(rx.size()); }
    int peek() { return rx.empty() ? -1 : rx.front(); }
    int read() { if(rx.empty()) return -1; int b=rx.front(); rx.erase(rx.begin()); return b; }
    size_t readBytes(uint8_t* out, size_t n) {
        size_t count=std::min(n,rx.size());
        std::copy(rx.begin(),rx.begin()+count,out); rx.erase(rx.begin(),rx.begin()+count); return count;
    }
    size_t write(const uint8_t* data, size_t length) {
        sent.emplace_back(data,data+length);
        if(useOverride) { rx=overrideReply; return length; }
        rx.clear();
        if(!linkUp || baud != 9600 || config != SERIAL_8N2) return length;
        if(length == 8 && data[1] == 3) {
            uint16_t addr=(uint16_t(data[2])<<8)|data[3];
            uint16_t count=(uint16_t(data[4])<<8)|data[5];
            if(simulateRpmDecay && addr==0x1000 && stopReadCountdown && --stopReadCountdown==0) registers[0x1000]=0;
            rx={data[0],3,uint8_t(count*2)};
            for(uint16_t i=0;i<count;++i) {
                auto item=registers.find(addr+i);
                if(item==registers.end()) { rx={data[0],0x83,2}; break; }
                rx.push_back(uint8_t(item->second>>8));rx.push_back(uint8_t(item->second));
            }
            uint16_t crc=modbus_crc16(rx.data(),int(rx.size()));
            rx.push_back(uint8_t(crc));rx.push_back(uint8_t(crc>>8));
        } else if(length==8 && data[1]==6) {
            uint16_t addr=(uint16_t(data[2])<<8)|data[3];
            uint16_t value=(uint16_t(data[4])<<8)|data[5];
            registers[addr >= 0x80 && addr < 0x100 ? addr-0x80 : addr] = value;
            if(simulateRpmDecay && addr==0x10F && value==0 && registers[0x1000]!=0) stopReadCountdown=2;
            rx.assign(data,data+length);
        }
        return length;
    }
};
extern FakeUart Serial2;
'''

DEFINITIONS_STUB = r'''

#pragma once
#include "Arduino.h"
constexpr uint16_t COMM_DELAY_SEND=60, COMM_DELAY_RECEIVE=50;
constexpr uint16_t COMM_DELAY_IDLE=1000, COMM_DELAY_MENU=1500, COMMINTERVAL=60;
constexpr int EXSDA=8, EXSCL=9;
'''

BOARD_STUB = r'''

#pragma once
enum class ControllerBoardId { AdafruitFeather, WaveshareRs485Can };
struct ControllerBoardPins { int8_t rs485Rxd=18, rs485Txd=17, rs485Re=21, rs485De=21; };
struct ControllerBoardInfo { ControllerBoardId id=ControllerBoardId::WaveshareRs485Can; };
inline const ControllerBoardPins& controller_board_pins() { static ControllerBoardPins pins; return pins; }
inline const ControllerBoardInfo& controller_board_info() { static ControllerBoardInfo info; return info; }
inline bool controller_board_pin_valid(int pin) { return pin>=0; }
'''

HAL_STUB = r'''
#pragma once
#include "motor_backend.h"
enum COMM_STATUS { COMM_DISCONNECTED, COMM_CONNECTED, COMM_INVALID_STATUS };
enum SYSTEM_STATUS { MOTOR_NOT_CONNECTED, MOTOR_NOT_READY, MOTOR_READY, MOTOR_ENABLED, MOTOR_FAULT, MOTOR_INVALID };
enum DriveType { DRIVE_TYPE_JMC, DRIVE_TYPE_TSE };
enum MotorCommandId { MOTOR_COMMAND_TORQUE_LIMIT };
struct MotorProbeReport { bool tse_signature=false; };
extern int16_t motor_setRPM, motor_currentRPM, motor_currentTorque, commCounter;
extern COMM_STATUS commStatus;
extern SYSTEM_STATUS currentStatus;
DriveType motor_get_drive_type();
int16_t motor_get_effective_set_rpm();
int16_t motor_get_formatted_command_value(MotorCommandId,int8_t);
'''

class KfmTransportTests(unittest.TestCase):
    def compile_and_run(self, locked, zero_probe=False, sanitize=False, explicitly_disabled=False, pa25_probe=False, pulse=False, touch=False, candidate=False, max_rpm=500, standalone=False):
        compiler=shutil.which("c++")
        if not compiler: self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as temp:
            d=Path(temp);(d/"TSE").mkdir()
            stubs={"Arduino.h":ARDUINO_STUB,"HardwareSerial.h":'#include "Arduino.h"\n',
                   "definitions.h":DEFINITIONS_STUB,"controller_board.h":BOARD_STUB,"motor_hal.h":HAL_STUB,
                   "display.h":'#pragma once\nextern bool disp_updateRequired;\n',
                   "main.h":'#pragma once\n#include "Arduino.h"\nenum States {IDLE,GRINDING,MENU1,IDLE_GBW,GRINDING_GBW,PURGING};\nextern States state;\nextern uint8_t error;\nextern bool newData;\nbool controller_is_menu_state(States);\n',
                   "espnow_control.h":'#pragma once\nextern bool fakePeerActive;\nextern unsigned long fakePeerAt;\ninline void espnow_control_poll_probe_status() {}\ninline bool espnow_control_is_active(){return fakePeerActive;}\ninline unsigned long espnow_control_last_command_rx_at(){return fakePeerAt;}\n',
                   "controller_ota_transport.h":'#pragma once\ninline bool controller_ota_transport_active() { return false; }\n'}
            for name,body in stubs.items(): (d/name).write_text(body)
            for name in ("comm.cpp","comm.h","motor_backend.h","motor_probe.cpp","motor_probe.h"):
                shutil.copyfile(ROOT/"src"/name,d/name)
            shutil.copyfile(ROOT/"src/TSE/motorcontrol_tse.cpp",d/"motorcontrol_tse.cpp")
            shutil.copyfile(ROOT/"src/TSE/motorcontrol_tse.h",d/"motorcontrol_tse.h")
            shutil.copyfile(ROOT/"src/TSE/motorcontrol_tse.h",d/"TSE/motorcontrol_tse.h")
            shutil.copyfile(ROOT/"src/SEW/movilink.cpp",d/"movilink.cpp")
            shutil.copyfile(ROOT/"src/TSE/kfm_zero_probe.h",d/"kfm_zero_probe.h")
            shutil.copyfile(ROOT/"src/TSE/kfm_zero_probe.h",d/"TSE/kfm_zero_probe.h")
            for name in ("kfm_pa25_probe.h","kfm_pa25_pulse.h"):
                shutil.copyfile(ROOT/"src/TSE"/name,d/name)
                shutil.copyfile(ROOT/"src/TSE"/name,d/"TSE"/name)
            output=d/"test"
            cmd=[compiler,"-std=c++17","-Wall","-Wextra","-Wno-unused-parameter","-I",str(d),"-I",str(ROOT/"src")]
            if sanitize: cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            if candidate: cmd += ["-DSG98_TSE_KFM_RUN_CANDIDATE=1","-DSG98_TSE_KFM_ALLOW_MOTION=1"]
            if candidate: cmd.append("-DSG98_TSE_KFM_MAX_RPM="+str(max_rpm))
            if standalone:cmd += ["-DSG98_TSE_KFM_STANDALONE=1","-DSG98_TSE_KFM_OPERATION_MS=120000"]
            if locked: cmd.append("-DSG98_TSE_KFM_COMMISSIONING=1")
            if explicitly_disabled: cmd.append("-DSG98_TSE_KFM_ZERO_PROBE=0")
            if touch: cmd.append("-DSG98_TSE_KFM_TOUCH_TEST=1")
            if pulse: cmd.append("-DSG98_TSE_KFM_PA25_PULSE=1")
            if pa25_probe: cmd.append("-DSG98_TSE_KFM_PA25_PROBE=1")
            if zero_probe: cmd.append("-DSG98_TSE_KFM_ZERO_PROBE=1")
            cmd.append(str(ROOT/"src/TSE/kfm_zero_probe.cpp"))
            cmd.append(str(ROOT/"src/TSE/kfm_pa25_probe.cpp"))
            cmd.append(str(ROOT/"src/TSE/kfm_pa25_pulse.cpp"))
            cmd += [str(d/name) for name in ("comm.cpp","motor_probe.cpp","motorcontrol_tse.cpp","movilink.cpp")]
            cmd += [str(ROOT/"tools/tests/tse"/("kfm_run_transport_harness.cpp" if candidate else "kfm_transport_harness.cpp")),"-o",str(output)]
            result=subprocess.run(cmd,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_run_candidate_transport(self): self.compile_and_run(True,candidate=True)
    def test_run_candidate_transport_with_sanitizers(self): self.compile_and_run(True,candidate=True,sanitize=True)
    def test_run_candidate_800rpm_transport(self): self.compile_and_run(True,candidate=True,max_rpm=800,sanitize=True)
    def test_standalone_transport(self): self.compile_and_run(True,candidate=True,max_rpm=2000,standalone=True,sanitize=True)
    def test_commissioning_transport(self): self.compile_and_run(True)
    def test_pa25_probe_transport(self): self.compile_and_run(True,pa25_probe=True)
    def test_pa25_probe_transport_with_sanitizers(self): self.compile_and_run(True,sanitize=True,pa25_probe=True)
    def test_pa25_pulse_transport(self): self.compile_and_run(True,pulse=True)
    def test_pa25_pulse_transport_with_sanitizers(self): self.compile_and_run(True,sanitize=True,pulse=True)
    def test_touch_transport(self): self.compile_and_run(True,pulse=True,touch=True)
    def test_touch_transport_with_sanitizers(self): self.compile_and_run(True,sanitize=True,pulse=True,touch=True)
    def test_general_transport(self): self.compile_and_run(False)
    def test_zero_probe_transport(self): self.compile_and_run(True,True)
    def test_explicitly_disabled_probe_is_unavailable(self): self.compile_and_run(True,False,False,True)
    def test_zero_probe_transport_with_sanitizers(self): self.compile_and_run(True,True,True)
if __name__=="__main__": unittest.main()
