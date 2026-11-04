"""Production scale-fault request/service against a deterministic BLE model."""
import subprocess,tempfile,unittest
from pathlib import Path
from tools.test_kfm_flows import function,ROOT
class ScaleLinkTests(unittest.TestCase):
 def test_fault_gate_and_hold(self):
  source=(ROOT/'src/gbw.cpp').read_text()
  funcs=function(source,'gbw_request_scale_link_test')+function(source,'gbw_service_scale_link_test')
  stub=r'''#include <atomic>
#include <cassert>
#include <cstdint>
#define SG98_TSE_KFM_RUN_CANDIDATE 1
namespace kfm_commissioning {constexpr int maxRpm=500;}
std::atomic<bool> scaleLinkTestRequested{false};
enum {IDLE_GBW,GRINDING_GBW,DRIVE_TYPE_TSE,OTHER_DRIVE,COMM_CONNECTED,SCALE_CONNECTED,SCALE_DISCONNECTED};
int state=IDLE_GBW,drive=DRIVE_TYPE_TSE,commStatus=COMM_CONNECTED,scaleStatus=SCALE_CONNECTED,motor_setRPM=200,motor_currentRPM=-200;bool scaleBleReady=true;
struct D{bool runCandidate=true,motionArmed=true;}diag;
const D& motor_tse_diagnostics(){return diag;} int motor_get_drive_type(){return drive;}
uint32_t clockMs=0;uint32_t millis(){return clockMs;}
struct {int drops=0;void disconnect(){++drops;}}scale;
void markScaleUnavailable(){scaleStatus=SCALE_DISCONNECTED;}
struct {void println(const char*){}}Serial;
'''
  main=r'''int main(){
 assert(!gbw_request_scale_link_test());state=GRINDING_GBW;
 diag.motionArmed=false;assert(!gbw_request_scale_link_test());diag.motionArmed=true;
 motor_currentRPM=0;assert(!gbw_request_scale_link_test());motor_currentRPM=-501;assert(!gbw_request_scale_link_test());motor_currentRPM=-200;
 drive=OTHER_DRIVE;assert(!gbw_request_scale_link_test());drive=DRIVE_TYPE_TSE;
 assert(gbw_request_scale_link_test());assert(!gbw_request_scale_link_test());assert(scale.drops==0);
 assert(gbw_service_scale_link_test());assert(scale.drops==1&&scaleStatus==SCALE_DISCONNECTED);assert(!gbw_request_scale_link_test());
 clockMs=4999;assert(gbw_service_scale_link_test());assert(scale.drops==1);
 clockMs=5000;assert(!gbw_service_scale_link_test());assert(scale.drops==1);
 scaleStatus=SCALE_CONNECTED;assert(gbw_request_scale_link_test());state=IDLE_GBW;
 assert(!gbw_service_scale_link_test());assert(scale.drops==1);state=GRINDING_GBW;assert(!gbw_service_scale_link_test());
 clockMs=UINT32_MAX-100;assert(gbw_request_scale_link_test());assert(gbw_service_scale_link_test());clockMs=4898;assert(gbw_service_scale_link_test());clockMs=4899;assert(!gbw_service_scale_link_test());assert(scale.drops==2);
}'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.cpp').write_text(stub+funcs+main)
   r=subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main()
