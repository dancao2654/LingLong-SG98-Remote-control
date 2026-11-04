"""Execute the actual remote item filtering/limit functions in a native model."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.test_kfm_flows import function,ROOT
class RemoteLimitTests(unittest.TestCase):
 def test_visible_capabilities_and_bounds(self):
  s=(ROOT/'sg98-remote/src/remote_app.cpp').read_text()
  functions=function(s,'settingItemVisible')+function(s,'filteredSettingItems')
  stub=r'''#include <string>
#include <cstring>
#include <cassert>
#include <cstdint>
using size_t=std::size_t;
enum{SETMAX=3,SETMOTORTORQUE=5,SETMOTORRAMP=6,AUTO_PURGE_ENABLED=1,SETPURGERPM=3,SETPURGEREVERSE=4,SETPURGESTABILTIME=8,SETPURGEFRAMESHIGH=9,SETPURGEFRAMESLOW=10,SETPURGEPRCTHIGH=11,SETPURGEPRCTLOW=12,AUTO_OFF_TIME=13};
struct SettingItem{uint8_t menuNum,itemIdx;int32_t maxValue;const char*action=nullptr;};
struct {struct{std::string actionToken;int32_t runMaxRpm=0,runMaxMs=0;}controllerStatus;int32_t menuValues[6][32]={};}app;
bool activeDriveIsJmc(){return false;}
template<class T,size_t N>constexpr size_t arrayCount(const T(&)[N]){return N;}
'''
  main=r'''int main(){
 SettingItem raw[]={{1,SETMAX,3000},{2,SETPURGERPM,3000},{2,SETPURGEREVERSE,3000},{2,AUTO_OFF_TIME,300},{2,AUTO_PURGE_ENABLED,1},{1,SETMOTORTORQUE,300},{0,0,0,"calibrate"},{2,SETPURGEFRAMESHIGH,20}};
 size_t count;auto items=filteredSettingItems(raw,8,&count);assert(count==8&&items[0].maxValue==3000);
 app.controllerStatus.actionToken="paired";app.controllerStatus.runMaxRpm=2000;app.controllerStatus.runMaxMs=120000;app.menuValues[1][SETMAX]=2000;
 items=filteredSettingItems(raw,8,&count);assert(count==4);assert(items[0].maxValue==2000&&items[1].maxValue==2000&&items[2].maxValue==2000&&items[3].maxValue==120);
 app.menuValues[1][SETMAX]=800;items=filteredSettingItems(raw,8,&count);assert(items[0].maxValue==2000&&items[1].maxValue==800);
 app.menuValues[1][SETMAX]=0;items=filteredSettingItems(raw,8,&count);assert(items[1].maxValue==0);
}'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.cpp').write_text(stub+functions+main)
   r=subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main()
