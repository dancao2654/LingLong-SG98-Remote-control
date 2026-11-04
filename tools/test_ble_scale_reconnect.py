"""Production BLE connection function with a deterministic NimBLE transport model.
No radio or USB is opened; handshakes are counted substitutes.
"""
import subprocess,tempfile,unittest
from pathlib import Path
from tools.test_kfm_flows import function,ROOT
STUB=r'''#include <cassert>
#include <string>
struct Address {std::string toString(){return "mac";}};
struct NimBLEAdvertisedDevice {Address getAddress(){return {};} int getAddressType(){return 1;} std::string getName(){return "BOOKOO_SC";}};
struct Client {bool connected=false,fail=false;int calls=0;bool connect(NimBLEAdvertisedDevice*,bool=true){++calls;if(connected||fail)return false;return connected=true;} bool connect(Address,bool=true){++calls;if(connected||fail)return false;return connected=true;} bool isConnected(){return connected;} void setClientCallbacks(void*,bool){} void setConnectionParams(int,int,int,int){} void setConnectTimeout(int){} void* getService(const char*s){return std::string(s)=="0FFE"?this:nullptr;} Address getPeerAddress(){return {};}};
using NimBLEClient=Client;
struct NimBLEDevice {inline static Client client;inline static bool existing=false,deleted=false;static int getCreatedClientCount(){return existing?1:0;} static Client* getClientByPeerAddress(Address){return existing?&client:nullptr;} static Client* getDisconnectedClient(){return nullptr;} static Client* createClient(){existing=true;return &client;} static void setMTU(int){} static void deleteClient(Client*){deleted=true;}};
struct {template<class...T>void printf(T...){} template<class T>void println(T){} template<class T>void print(T){}} Serial;
enum class ScaleType {UNKNOWN,ACAIA_NEW,ACAIA_OLD,BOOKOO,FELIC};
struct BLEScale {NimBLEAdvertisedDevice adv;NimBLEAdvertisedDevice*advDevice=&adv;Client*_pClient=nullptr;void*_pChrREAD=nullptr,*_pChrWRITE=nullptr;bool _debug=false,_connected=false,_isConnecting=false;int _clientCallbacks=0,handshakes=0;ScaleType _type=ScaleType::UNKNOWN;std::string connected_name,connected_mac;void performHandshakeAcaia(bool){++handshakes;}void performHandshakeBookoo(){++handshakes;}void performHandshakeFelicita(){++handshakes;}bool connectToServer();};
'''
class ReconnectTests(unittest.TestCase):
 def test_connection_lifecycle(self):
  source=function((ROOT/'lib/BLEScale/src/BLEScale.cpp').read_text(),'BLEScale::connectToServer')
  main=r'''int main(){
for(int scenario=0;scenario<4;++scenario){
 NimBLEDevice::client=Client{};NimBLEDevice::existing=scenario!=0;NimBLEDevice::deleted=false;BLEScale scale;
 if(scenario==2)NimBLEDevice::client.connected=true;
 if(scenario==3){NimBLEDevice::client.fail=true;scale._pClient=&NimBLEDevice::client;scale._connected=true;}
 bool ok=scale.connectToServer();
 if(scenario==3){assert(!ok);assert(NimBLEDevice::deleted);assert(!scale._pClient&&!scale._connected);}
 else {assert(ok);assert(scale.handshakes==1);assert(NimBLEDevice::client.calls==(scenario==2?0:1));}
}
}'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.cpp').write_text(STUB+source+main)
   r=subprocess.run(['c++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(p/'test')],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main()
