"""Host tests for the paired physical-gesture idempotency protocol; no hardware."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class IntentTests(unittest.TestCase):
 def test_ledger_and_client(self):
  code=r'''#include "sg98_intent_token.h"
#include <cassert>
using namespace sg98_intent;
int main(){
 Ledger server;server.seed(0x123456789abcdef0ULL);Client remote;char offer[25],start[25],stop[25],secondStop[25];server.offer(offer);
 assert(remote.next(offer,start));assert(server.begin(start,'S')==Decision::New);server.finish(true);
 assert(server.begin(start,'S')==Decision::DuplicateAccepted);
 // Real second press gets a new ID even before the newer status arrives.
 assert(remote.next(offer,stop));assert(std::strcmp(start,stop)!=0);
 assert(server.begin(stop,'T')==Decision::New);server.finish(true);
 assert(server.begin(stop,'T')==Decision::DuplicateAccepted);
 assert(remote.next(offer,secondStop));assert(server.begin(secondStop,'T')==Decision::New);server.finish(true);
 assert(server.begin(start,'S')==Decision::Invalid); // delayed old Start cannot restart
 Ledger lost;lost.seed(0x123456789abcdef0ULL);
 assert(lost.begin(stop,'T')==Decision::New);lost.finish(true);
 assert(lost.begin(start,'S')==Decision::Invalid); // Stop supersedes a lost Start
 server.offer(offer);assert(remote.next(offer,start));assert(server.begin(start,'S')==Decision::New);server.finish(false);
 assert(server.begin(start,'S')==Decision::DuplicateRejected); // rejected Start never becomes armed later
 Ledger reboot;reboot.seed(0x9999999999999999ULL);assert(reboot.begin(start,'S')==Decision::Invalid);
 assert(server.begin("",'S')==Decision::Invalid);assert(server.begin(nullptr,'S')==Decision::Invalid);
 assert(!remote.next("bad",start));
 server.reset(0x1212121212121212ULL);assert(server.begin(start,'S')==Decision::Invalid);
 // Sequence exhaustion fails closed; no wrap into an old accepted gesture.
 char last[25];format(last,0x9999999999999999ULL,0xffffffffU);
 assert(reboot.begin(last,'T')==Decision::New);reboot.finish(true);reboot.offer(offer);assert(!offer[0]);
}'''
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'test.cpp').write_text(code)
   r=subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I',str(ROOT/'lib/sg98_remote_protocol/src'),str(p/'test.cpp'),'-o',str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
if __name__=='__main__':unittest.main()
