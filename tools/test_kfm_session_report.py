import csv
import tempfile
import unittest
from pathlib import Path
from tools.kfm_session_report import analyze,write_report,load_events

def event(at,kind,**fields):return {'elapsedSeconds':at,'utc':'2026-10-27T20:00:00Z','object':{'type':kind,**fields}}
def status(at,**fields):return event(at,'status',state='IDLE',motorSetRPM=0,currentRPM=0,comm='CONNECTED',error=0,**fields)
class ReportTests(unittest.TestCase):
    def test_stale_and_identity_proof(self):
        data=[event(0,'tse_run_candidate',available=True,stopReadbackVerified=True),status(1),status(4),event(5,'device',mac='x'),status(5.1)]
        rows=analyze(data)['rows'];self.assertIs(rows[0]['stopReadbackVerified'],True)
        self.assertIsNone(rows[1]['stopReadbackVerified']);self.assertIsNone(rows[2]['stopReadbackVerified'])
    def test_reason_order_and_no_physical_pass(self):
        d=analyze([event(1,'tse',reason='remote_link_lost'),event(2,'tse',reason='motion_locked'),status(2.1)])
        self.assertEqual([v['reason'] for v in d['reasonChanges']],['remote_link_lost','motion_locked'])
        self.assertNotIn('passed',d)
    def test_zero_proof_needs_current_zero(self):
        e=status(1);e['object']['currentRPM']=-100
        d=analyze([event(0,'tse_run_candidate',available=True,stopReadbackVerified=True),e]);self.assertFalse(d['rows'][0]['stopReadbackVerified'])
        self.assertEqual(d['reportedPeakAbsRPM'],100)
    def test_unavailable_touch_view_is_not_a_radio_failure(self):
        d=analyze([event(0,'tse_touch_test',available=False,remoteFresh=False),status(1)])
        self.assertIsNone(d['rows'][0]['remoteFresh'])
    def test_export_and_malformed_input(self):
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp);(p/'events.jsonl').write_text('not json\n'+__import__('json').dumps(event(0,'tse',reason='=BAD()'))+'\n'+__import__('json').dumps(status(1))+'\n')
            d=write_report(load_events(p/'events.jsonl'),p/'report');self.assertEqual(d['ignoredMalformedRecords'],1)
            with (p/'report/timeline.csv').open() as handle:rows=list(csv.DictReader(handle))
            self.assertEqual(rows[0]['backendReason'],"'=BAD()")
            with self.assertRaises(FileExistsError):write_report([],p/'report')
if __name__=='__main__':unittest.main()
