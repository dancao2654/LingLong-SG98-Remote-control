import copy
import unittest
from tools.kfm_pa25_pulse_operator import OperatorGate,START,CANCEL,RESTORE,restoration_confirmed

class OperatorGateTests(unittest.TestCase):
    def ready(self):
        gate=OperatorGate('B8:1F:3F:C8:F5:30')
        samples=[{'type':'device','role':'controller','target':'waveshare_rs485_can','buildEnv':'controller-kfm-pa25-pulse','firmwareVersion':'2026.10.27.7','mac':'B8:1F:3F:C8:F5:30'},
                 {'type':'status','state':'IDLE','comm':'CONNECTED','error':0,'currentRPM':0,'motorSetRPM':0},
                 {'type':'tse','phase':'complete','kfm':True,'identified':True,'motionArmed':False,'monitorValid':True,'alarmValid':True},
                 {'type':'tse_pa25_pulse','available':True,'phase':'idle'}]
        for item in samples:gate.observe(item,100)
        return gate,samples
    def test_ack_without_measured_zero_readback_is_not_restoration(self):
        good={'type':'tse_pa25_pulse','available':True,'restoreAcknowledged':True,'restoreReadbackValid':True,'latestValid':True}
        self.assertTrue(restoration_confirmed(good))
        for key in ('available','restoreAcknowledged','restoreReadbackValid','latestValid'):
            bad=good.copy();bad[key]=False
            self.assertFalse(restoration_confirmed(bad))
            del bad[key]
            self.assertFalse(restoration_confirmed(bad))
        self.assertFalse(restoration_confirmed(None))
        self.assertFalse(restoration_confirmed({'restoreAcknowledged':True}))
    def test_only_named_probe_and_cancel_commands(self):
        gate,_=self.ready()
        self.assertEqual(gate.command('pulse',101),(START,None))
        self.assertEqual(gate.command('cancel',101),(CANCEL,None))
        self.assertEqual(gate.command('restore',101),(RESTORE,None))
        for word in ('api action start','mwu 9600 8N2 1 24 1000','pulse 1000','start','arm'):
            with self.assertRaises(ValueError):gate.command(word,101)
        self.assertEqual(gate.command('off',101),(None,'operator_confirmed_main_switch_off'))
        self.assertEqual(gate.command('on',101),(None,'operator_confirmed_main_switch_on'))
    def test_stale_or_unsafe_state_is_refused(self):
        gate,samples=self.ready()
        with self.assertRaises(ValueError):gate.command('pulse',104)
        for index,key,value in [(1,'currentRPM',2),(1,'error',3),(1,'state','GRINDING'),(1,'comm','DISCONNECTED'),
                                (2,'motionArmed',True),(2,'monitorValid',False),(2,'kfm',False),
                                (3,'available',False),(3,'phase','waiting_for_power_on')]:
            gate,_=self.ready();bad=copy.deepcopy(samples[index]);bad[key]=value;gate.observe(bad,101)
            with self.assertRaises(ValueError):gate.command('pulse',101)
            self.assertEqual(gate.command('cancel',101),(CANCEL,None))
    def test_identity_change_revokes_an_earlier_verified_session(self):
        gate,samples=self.ready()
        wrong=copy.deepcopy(samples[0]);wrong['mac']='44:BD:8D:79:9D:DC'
        with self.assertRaises(RuntimeError):gate.observe(wrong,101)
        with self.assertRaises(ValueError):gate.command('pulse',101)
        gate.observe(samples[0],102)
        with self.assertRaises(ValueError):gate.command('pulse',102)
    def test_wrong_board_mac_or_build_never_starts(self):
        _,samples=self.ready()
        for key,value in [('role','remote'),('mac','44:BD:8D:79:9D:DC'),('buildEnv','controller-kfm-commissioning'),('firmwareVersion','2026.10.27.3'),('firmwareVersion','2026.10.27.5')]:
            gate=OperatorGate('B8:1F:3F:C8:F5:30');bad=copy.deepcopy(samples[0]);bad[key]=value
            with self.assertRaises(RuntimeError):gate.observe(bad,100)
            with self.assertRaises(ValueError):gate.command('pulse',101)

if __name__=='__main__':unittest.main()
