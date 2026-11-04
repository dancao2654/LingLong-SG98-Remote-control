"""Compile production action/purge/GBW functions against the real KFM backend.

No ports opened. UI/BLE transport and gbw_learn are substitutes; scheduler and
motor coast are deterministic models. This does not prove physical behaviour.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    match=re.search(r'(?m)^[\w*&:<>, \t]+\b'+re.escape(name)+r'\s*\([^;{}]*\)\s*\{',source)
    if not match: raise ValueError('Production function missing: '+name)
    depth=0
    for token in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|[{}]',source[match.end()-1:]):
        v=token.group()
        if v=='{':depth+=1
        elif v=='}':
            depth-=1
            if depth==0:return source[match.start():match.end()-1+token.end()]+'\n'
    raise ValueError('Unbalanced production function '+name)

def production_header():
    app=(ROOT/'src/main.cpp').read_text();gbw=(ROOT/'src/gbw.cpp').read_text();diag=(ROOT/'src/diag_console.cpp').read_text();head=(ROOT/'src/main.h').read_text()
    result=head[head.index('enum Menu1Items'):head.index('extern menuEntry')]
    result+='\nmenuEntry Menu1[NUM_MENU1_ITEMS]={},Menu2[NUM_MENU2_ITEMS]={},Menu3[NUM_MENU3_ITEMS]={};\n'
    result+='\nint8_t menu1Selected=EXITMENU,menu2Selected=RETURN_FROM_PURGE,menu3Selected=RETURN_FROM_GBW,menu4Selected=RETURN_FROM_JMC_BAUD,menu5Selected=RETURN_FROM_WIRELESS;bool enterMenu=false;\n'
    result+='\n'+app[app.index('constexpr uint16_t PURGE_VIBRATION_MIN_MS'):app.index('bool idleSleepActive')]
    result+='\n'+gbw[gbw.index('unsigned long startOfShot'):gbw.index('namespace {')]
    result+='\nbool shotHistoryTruncated=false;bool gbwStartRequested=false;bool gbwPredictionResetRequested=false;\n'
    result+=function(gbw,'append_shot_sample')
    result+='\nint32_t gbw_predict();\nvoid purge_cancel_pending();\n'
    for name in ('purgeCalibrationIndexForRpm','purgeBaselineTorqueForRpm','purgeScalarForRpm','purgeManualThresholdForRpm','purgeClampedRpm','purgeVibrationDurationMs','purgeVibrationStepFor','purgeVibrationRpm','applyPurgeConstantWindow','applyPurgeVibrationWindow','purge_prepare_for_grinding','purge_cancel_pending','purge_mark_started','finishPurgeSequence','checkPurge','do_purge'):
        result+=function(app,name)
    for name in ('abs_mg','clamp_gbw_rpm','gbw_liveRateMgPerMs','gbw_predict','gbw_prepare_for_grinding','do_gbw'):result+=function(gbw,name)
    for name in ('apiActionStart','apiActionStop','apiActionIdle','apiIntentAction','apiIntentIdle'):result+=function(diag,name)
    return result

class FlowTests(unittest.TestCase):
    def test_production_flows(self): self.run_flows(False)
    def test_production_flows_sanitized(self): self.run_flows(True)
    def test_standalone_flows(self): self.run_flows(False,True)
    def test_standalone_flows_sanitized(self): self.run_flows(True,True)
    def run_flows(self,sanitized,standalone=False):
        compiler=shutil.which('c++')
        if not compiler:self.skipTest('C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='kfm-flows-') as temp:
            d=Path(temp);(d/'production_flows.h').write_text(production_header());(d/'production_flow_declarations.h').write_text('');out=d/'flows'
            flags=['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ALLOW_MOTION=1','-DSG98_TSE_KFM_RUN_CANDIDATE=1']
            if standalone:flags+=['-DSG98_TSE_KFM_STANDALONE=1','-DSG98_TSE_KFM_MAX_RPM=2000','-DSG98_TSE_KFM_OPERATION_MS=120000']
            if sanitized:flags+=['-fsanitize=address,undefined,float-cast-overflow','-fno-omit-frame-pointer','-g']
            cmd=[compiler,'-std=c++17','-Wno-return-type','-I',str(d),'-I',str(ROOT/'tools/tests/tse/stubs'),'-I',str(ROOT/'src'),'-I',str(ROOT/'lib/sg98_remote_protocol/src'),*flags,str(ROOT/'src/TSE/motorcontrol_tse.cpp'),str(ROOT/'tools/tests/tse/kfm_flow_harness.cpp'),'-o',str(out)]
            r=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            cases=('standalone','standalone-intents') if standalone else ('manual','purge','radio','gbw-target','gbw-empty','gbw-disconnect','gbw-overflow','gbw-restart','gbw-link-loss','gbw-no-scale','manual-timeout','gbw-tare-empty')
            for case in cases:
                with self.subTest(case=case,sanitized=sanitized):
                    r=subprocess.run([str(out),case],capture_output=True,text=True,timeout=20)
                    self.assertEqual(r.returncode,0,r.stdout+r.stderr)
if __name__=='__main__':unittest.main()
