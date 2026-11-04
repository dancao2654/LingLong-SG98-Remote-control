import copy
import unittest
from tools.kfm_radio_off_test import cut_allowed,radio_stop_ok,recovery_safe,ALLOWED

class RadioOffChecks(unittest.TestCase):
    def fixtures(self):
        t={'type':'tse_touch_test','runActive':True,'armed':False,'remoteFresh':True}
        p={'type':'tse_pa25_pulse','phase':'running','latestValid':True,'startedAtMs':200,'latestSon':1,'latestSelect':2,'latestPa25':-100,'latestAlarm':0,'latestRpm':-100}
        return t,p
    def test_only_new_measured_motion_triggers_cut(self):
        t,p=self.fixtures();self.assertTrue(cut_allowed(t,p,100))
        self.assertFalse(cut_allowed(t,p,200));self.assertFalse(cut_allowed(t,p,None))
        for key,val in [('phase','complete'),('latestValid',False),('latestSon',0),('latestSelect',0),('latestPa25',0),('latestAlarm',3),('latestRpm',0),('latestRpm',100),('latestRpm',-200)]:
            bad=p.copy();bad[key]=val;self.assertFalse(cut_allowed(t,bad,100))
        t['runActive']=False;self.assertFalse(cut_allowed(t,p,100))
    def test_timer_or_old_run_cannot_count_as_radio_stop(self):
        t,p=self.fixtures();t.update(reason='remote_link_lost',remoteFresh=False,runActive=False)
        p.update(phase='cancelled',available=True,restoreAcknowledged=True,restoreReadbackValid=True,latestRpm=0,latestPa25=0,latestSon=0)
        s={'state':'IDLE','currentRPM':0,'motorSetRPM':0,'comm':'CONNECTED','error':0}
        self.assertTrue(radio_stop_ok(t,p,s,200));self.assertFalse(radio_stop_ok(t,p,s,100))
        for key,val in [('phase','complete'),('restoreAcknowledged',False),('restoreReadbackValid',False),('latestRpm',1)]:
            bad=p.copy();bad[key]=val;self.assertFalse(radio_stop_ok(t,bad,s,200))
        t['reason']='pulse_completed_motion_observed';self.assertFalse(radio_stop_ok(t,p,s,200))
    def test_recovery_requires_disarmed_stationary_data(self):
        t={'remoteFresh':True,'armed':False,'runActive':False};s={'state':'IDLE','motorSetRPM':0,'currentRPM':0,'error':0,'comm':'CONNECTED'}
        self.assertTrue(recovery_safe(t,s))
        for key,val in [('remoteFresh',False),('armed',True),('runActive',True)]:
            bad=t.copy();bad[key]=val;self.assertFalse(recovery_safe(bad,s))
        s['currentRPM']=100;self.assertFalse(recovery_safe(t,s))
    def test_no_start_or_speed_command_from_host(self):
        self.assertNotIn('api action start',ALLOWED)
        self.assertFalse(any('pulse start' in x or x.startswith('mwu') for x in ALLOWED))

if __name__=='__main__':unittest.main()
