#!/usr/bin/env python3
"""Offline KFM regression runner. No USB devices, network or firmware flashing."""
from __future__ import annotations
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    compiler=shutil.which('c++')
    if not compiler: parser.error('A host C++ compiler is required')
    results=[]
    def execute(label,cmd,expect_failure=False,input_text=None,expected_error=None):
        start=time.monotonic()
        try:
            r=subprocess.run(cmd,cwd=ROOT,input=input_text,capture_output=True,text=True,timeout=180)
            ok=(r.returncode!=0) if expect_failure else (r.returncode==0)
            if expect_failure: ok=ok and 'error:' in r.stderr and (expected_error is None or expected_error in r.stderr)
            result={'label':label,'passed':ok,'elapsedSeconds':round(time.monotonic()-start,3),'exitCode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        except subprocess.TimeoutExpired as error:
            result={'label':label,'passed':False,'elapsedSeconds':180,'error':'timeout'}
        results.append(result)
        print(('PASS' if result['passed'] else 'FAIL')+' '+label,flush=True)
        if not result['passed']:print(result.get('stdout','')+result.get('stderr',''),flush=True)
        return result['passed']
    with tempfile.TemporaryDirectory(prefix='sg98-kfm-checks-') as temp:
        d=Path(temp)
        def compile_run(label,sources,flags=(),stubs=False):
            out=d/label
            cmd=[compiler,'-std=c++17','-Wall','-Wextra','-Wno-unused-parameter']
            if stubs:cmd+=['-I',str(ROOT/'tools/tests/tse/stubs')]
            cmd+=['-I',str(ROOT/'src'),*flags,*[str(ROOT/name) for name in sources],'-o',str(out)]
            if execute(label+' compile',cmd):execute(label,[str(out)])
        backend=['src/TSE/motorcontrol_tse.cpp','tools/tests/tse/test_tse_backend.cpp']
        compile_run('tse-family',backend,stubs=True)
        candidate_flags=['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ALLOW_MOTION=1','-DSG98_TSE_KFM_RUN_CANDIDATE=1']
        compile_run('kfm-run-candidate',[backend[0],'tools/tests/tse/test_kfm_run_candidate.cpp'],candidate_flags,stubs=True)
        compile_run('kfm-run-candidate-800',[backend[0],'tools/tests/tse/test_kfm_run_candidate.cpp'],candidate_flags+['-DSG98_TSE_KFM_MAX_RPM=800'],stubs=True)
        compile_run('kfm-policy-run-candidate-800',['tools/tests/tse/test_kfm_policy.cpp'],candidate_flags+['-DSG98_TSE_KFM_MAX_RPM=800'])
        standalone_flags=candidate_flags+['-DSG98_TSE_KFM_STANDALONE=1','-DSG98_TSE_KFM_MAX_RPM=2000','-DSG98_TSE_KFM_OPERATION_MS=120000']
        compile_run('kfm-policy-standalone',['tools/tests/tse/test_kfm_policy.cpp'],standalone_flags)
        execute('reject-2000-without-standalone',[compiler,'-std=c++17','-I',str(ROOT/'src'),'-DSG98_TSE_KFM_MAX_RPM=2000','-x','c++','-c','-','-o',str(d/'max-bad.o')],True,'#include "TSE/kfm_commissioning_policy.h"\n','KFM supervised test speed')
        compile_run('kfm-policy-run-candidate',['tools/tests/tse/test_kfm_policy.cpp'],candidate_flags)
        compile_run('kfm-locked',[backend[0],'tools/tests/tse/test_kfm_backend.cpp'],stubs=True)
        compile_run('kfm-motion-simulation',[backend[0],'tools/tests/tse/test_kfm_backend.cpp'],['-DSG98_TSE_KFM_ALLOW_MOTION=1'],stubs=True)
        compile_run('kfm-zero-probe',['src/TSE/kfm_zero_probe.cpp','tools/tests/tse/test_kfm_zero_probe.cpp'])
        compile_run('kfm-pa25-probe',['src/TSE/kfm_pa25_probe.cpp','tools/tests/tse/test_kfm_pa25_probe.cpp'])
        compile_run('kfm-policy-pa25',['tools/tests/tse/test_kfm_policy.cpp'],['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_PA25_PROBE=1'])
        compile_run('kfm-pa25-pulse',['src/TSE/kfm_pa25_pulse.cpp','tools/tests/tse/test_kfm_pa25_pulse.cpp'])
        compile_run('kfm-policy-pulse',['tools/tests/tse/test_kfm_policy.cpp'],['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_PA25_PULSE=1'])
        for label,flags in [('candidate-without-gate',['-DSG98_TSE_KFM_ALLOW_MOTION=1']),('candidate-without-motion',['-DSG98_TSE_KFM_COMMISSIONING=1'])]:
            execute(label,[compiler,'-std=c++17','-I',str(ROOT/'src'),'-DSG98_TSE_KFM_RUN_CANDIDATE=1',*flags,'-x','c++','-c','-','-o',str(d/(label+'.o'))],True,'#include "TSE/kfm_commissioning_policy.h"\n','Run candidate requires')
        for zero in (False,True):
            flags=['-DSG98_TSE_KFM_COMMISSIONING=1']+(['-DSG98_TSE_KFM_ZERO_PROBE=1'] if zero else [])
            compile_run('kfm-policy-'+('zero-probe' if zero else 'locked'),['tools/tests/tse/test_kfm_policy.cpp'],flags)
        text='#include "TSE/kfm_commissioning_policy.h"\n'
        for label,flags,needle in [('reject-probe-with-motion',['-DSG98_TSE_KFM_ZERO_PROBE=1','-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ALLOW_MOTION=1'],'must not be combined'),
                            ('reject-probe-without-gate',['-DSG98_TSE_KFM_ZERO_PROBE=1'],'requires the commissioning transport gate')]:
            execute(label,[compiler,'-std=c++17','-I',str(ROOT/'src'),*flags,'-x','c++','-c','-','-o',str(d/(label+'.o'))],True,text,needle)
        for label,flags in [('pa25-with-motion',['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ALLOW_MOTION=1']),('pa25-without-gate',[]),('pa25-with-son-probe',['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ZERO_PROBE=1'])]:
            execute(label,[compiler,'-std=c++17','-I',str(ROOT/'src'),'-DSG98_TSE_KFM_PA25_PROBE=1',*flags,'-x','c++','-c','-','-o',str(d/(label+'.o'))],True,text,'PA25 audit requires')
        for label,flags in [('pulse-with-motion',['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ALLOW_MOTION=1']),('pulse-without-gate',[]),('pulse-with-son-probe',['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_ZERO_PROBE=1']),('pulse-with-marker',['-DSG98_TSE_KFM_COMMISSIONING=1','-DSG98_TSE_KFM_PA25_PROBE=1'])]:
            execute(label,[compiler,'-std=c++17','-I',str(ROOT/'src'),'-DSG98_TSE_KFM_PA25_PULSE=1',*flags,'-x','c++','-c','-','-o',str(d/(label+'.o'))],True,text,'PA25 pulse requires')
        execute('touch-without-pulse',[compiler,'-std=c++17','-I',str(ROOT/'src'),'-DSG98_TSE_KFM_TOUCH_TEST=1','-DSG98_TSE_KFM_COMMISSIONING=1','-x','c++','-c','-','-o',str(d/'touch-bad.o')],True,text,'Touch test requires')
    execute('production-UART-and-operator'   ,[sys.executable,'-m','unittest','tools.test_kfm_remote_limits','tools.test_kfm_intent','tools.test_kfm_scale_link','tools.test_ble_scale_reconnect','tools.test_kfm_transport','tools.test_kfm_flows','tools.test_kfm_session_report','tools.test_kfm_operator','tools.test_kfm_pa25_operator','tools.test_kfm_pulse_operator','tools.test_kfm_touch_operator','tools.test_kfm_radio_off','-v'])
    report={'utc':datetime.now(timezone.utc).isoformat(),'hardwareOpened':False,'results':results,'passed':all(r['passed'] for r in results)}
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
    print('All offline checks passed.' if report['passed'] else 'Some checks failed.')
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
