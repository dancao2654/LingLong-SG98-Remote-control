#!/usr/bin/env python3
"""Supervised controller-radio failure injection on the existing .8 touch build.

Only arm is sent: the human must press touchscreen Start. At measured motion the
host disables the controller's ESP-NOW interface, verifies the radio-loss stop,
then restores radio only after a verified zero state. This is NOT a measurement
of the three-second missing-packet timeout with the receiver still active.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from queue import Queue, Empty
import threading
import time
import sys
try:
    from .kfm_touch_test_operator import OperatorGate, START, CANCEL, restoration_confirmed
    from .kfm_capture import READ_ONLY_COMMANDS, utc_now, summarize
except ImportError:
    from kfm_touch_test_operator import OperatorGate, START, CANCEL, restoration_confirmed
    from kfm_capture import READ_ONLY_COMMANDS, utc_now, summarize
RADIO_OFF='dbg espnow off'
RADIO_ON='dbg espnow on'
ALLOWED=(*READ_ONLY_COMMANDS,START,CANCEL,RADIO_OFF,RADIO_ON,"dbg status")


def cut_allowed(touch, pulse, baseline):
    return (isinstance(touch,dict) and isinstance(pulse,dict) and baseline is not None
        and touch.get('runActive') is True and touch.get('armed') is False
        and pulse.get('phase')=='running' and pulse.get('latestValid') is True
        and isinstance(pulse.get('startedAtMs'),int) and pulse['startedAtMs']!=baseline
        and pulse.get('latestSon')==1 and pulse.get('latestSelect')==2
        and pulse.get('latestPa25')==-100 and pulse.get('latestAlarm')==0
        and isinstance(pulse.get('latestRpm'),int) and -150<=pulse['latestRpm']<=-50)


def radio_stop_ok(touch,pulse,status,run_id):
    return (isinstance(touch,dict) and isinstance(pulse,dict) and isinstance(status,dict)
        and run_id is not None and pulse.get('startedAtMs')==run_id
        and touch.get('reason')=='remote_link_lost' and touch.get('remoteFresh') is False
        and touch.get('armed') is False and touch.get('runActive') is False
        and pulse.get('phase')=='cancelled' and restoration_confirmed(pulse)
        and pulse.get('latestRpm')==0 and pulse.get('latestPa25')==0
        and pulse.get('latestSon')==0 and pulse.get('latestSelect')==2 and pulse.get('latestAlarm')==0
        and status.get('state')=='IDLE' and status.get('currentRPM')==0 and status.get('motorSetRPM')==0
        and status.get('comm')=='CONNECTED' and status.get('error')==0)


def recovery_safe(touch,status):
    return (isinstance(touch,dict) and isinstance(status,dict)
        and touch.get('remoteFresh') is True and touch.get('armed') is False and touch.get('runActive') is False
        and status.get('state')=='IDLE' and status.get('motorSetRPM')==0 and status.get('currentRPM')==0
        and status.get('error')==0 and status.get('comm')=='CONNECTED')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--expected-mac',required=True)
    parser.add_argument('--operator-present',action='store_true')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if not args.operator_present:parser.error('--operator-present is required')
    import serial
    output=args.output or Path('outputs')/('kfm-radio-off-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
    output.mkdir(parents=True,exist_ok=False)
    gate=OperatorGate(args.expected_mac)
    options=dict(port=None,baudrate=115200,timeout=.03)
    if os.name!='nt':options['exclusive']=True
    device=serial.Serial(**options);device.dtr=True;device.rts=False;device.port=args.port
    commands=Queue()
    def stdin_reader():
        for line in sys.stdin:commands.put(line.strip())
        commands.put('quit')
    threading.Thread(target=stdin_reader,daemon=True).start()
    started=time.monotonic();events=[];partial=bytearray()
    metadata={'startedUtc':utc_now(),'port':args.port,'expectedMac':args.expected_mac,
              'expectedVersion':'2026.10.27.8','operatorPresent':True,'commandsAllowed':ALLOWED,
              'faultKind':'controller_espnow_interface_off','missing_packet_timeout_measured':False,
              'armSent':False,'faultInjected':False,'stopProofVerified':False,'radioRestoreSent':False,
              'recoveryNoRestartVerified':False,'error':None}
    baseline=None;run_id=None;arm_accepted=False;fault_at=None;restored_at=None
    recovery_begin=None;recovery_samples=0;done=False;cancel_ack=False;host_aborted=False
    exit_code=0
    with (output/'raw.bin').open('wb') as raw,(output/'events.jsonl').open('w') as journal:
        def record(direction,**fields):
            e={'utc':utc_now(),'elapsedSeconds':round(time.monotonic()-started,4),'direction':direction,**fields}
            events.append(e);journal.write(json.dumps(e,ensure_ascii=False)+'\n');journal.flush()
        def send(command):
            nonlocal cancel_ack
            assert command in ALLOWED
            if command==CANCEL:cancel_ack=False
            device.write((command+'\n').encode('ascii'));record('TX',command=command)
        def read_once():
            nonlocal baseline,run_id,arm_accepted,fault_at,restored_at,recovery_begin,recovery_samples,done,cancel_ack
            chunk=device.read(8192)
            if not chunk:return
            raw.write(chunk);raw.flush();partial.extend(chunk)
            while b'\n' in partial:
                line,_,rest=partial.partition(b'\n');partial[:]=rest
                text=line.decode('utf8','replace').rstrip('\r')
                try:o=json.loads(text)
                except ValueError:record('RX',text=text);continue
                record('RX',object=o);gate.observe(o,time.monotonic())
                if not isinstance(o,dict):continue
                if o.get('type')=='ack':
                    if o.get('cmd')==START.removeprefix('api ') and o.get('ok') is True:arm_accepted=True
                    if o.get('cmd')==CANCEL.removeprefix('api ') and o.get('ok') is True:cancel_ack=True
                if o.get('type')=='dbg' and metadata['faultInjected']:
                    metadata['radioRestoreStatus' if metadata['radioRestoreSent'] else 'radioOffStatus']=o
                if o.get('type')!='tse_pa25_pulse':continue
                if baseline is None and o.get('phase') in ('idle','complete','cancelled','failed'):baseline=o.get('startedAtMs')
                now=time.monotonic();ts,touch=gate.latest.get('tse_touch_test',(-100,{}));ss,status=gate.latest.get('status',(-100,{}))
                if metadata['armSent'] and arm_accepted and not metadata['faultInjected'] and not host_aborted and now-ts<1.5 and cut_allowed(touch,o,baseline):
                    run_id=o['startedAtMs'];fault_at=now;metadata['faultInjected']=True;metadata['runId']=run_id
                    (output/'before-radio-off.json').write_text(json.dumps({'touch':touch,'pulse':o,'status':status},indent=2))
                    send(RADIO_OFF);send('dbg status');print('Measured motion: controller radio OFF requested.',flush=True)
                if metadata['faultInjected'] and not metadata['stopProofVerified'] and not host_aborted and metadata.get('radioOffStatus',{}).get('espnowActive') is False and now-ts<1.5 and now-ss<1.5 and radio_stop_ok(touch,o,status,run_id):
                    metadata['stopProofVerified']=True
                    (output/'radio-stop-proof.json').write_text(json.dumps({'touch':touch,'pulse':o,'status':status,'beforeHostDisarm':True},indent=2))
                    record('HOST',event='radio_loss_stop_zero_state_verified');print('Radio-loss stop and zero readback VERIFIED; restoring radio.',flush=True)
                    send(RADIO_ON);send('dbg status');metadata['radioRestoreSent']=True;restored_at=now
                if arm_accepted and not metadata['faultInjected'] and not host_aborted and baseline is not None and o.get('startedAtMs')!=baseline and o.get('phase') in ('complete','failed','cancelled'):
                    raise RuntimeError('Run ended before valid radio injection; no radio-stop conclusion')
                if metadata['radioRestoreSent'] and metadata['stopProofVerified'] and not host_aborted:
                    if now-ts<1.5 and now-ss<1.5 and o.get('startedAtMs')==run_id and recovery_safe(touch,status):
                        if recovery_begin is None:recovery_begin=now;recovery_samples=0
                        recovery_samples+=1
                        if recovery_samples>=3 and now-recovery_begin>=2:
                            metadata['recoveryNoRestartVerified']=True
                            (output/'radio-recovery-proof.json').write_text(json.dumps({'touch':touch,'pulse':o,'status':status,'samples':recovery_samples,'spanSeconds':now-recovery_begin},indent=2))
                            print('Radio link returned; no rearm or restart observed. Test complete.',flush=True);done=True
                    else:recovery_begin=None;recovery_samples=0
            if len(partial)>65536:record('RX',discardedUnterminatedLineBytes=len(partial));partial.clear()
        try:
            device.open();print('Radio-off fault test. Human at MAIN SWITCH required. Input: arm | disarm | quit.',flush=True)
            print('Log:',output.resolve(),flush=True)
            send('api device');next_identity=time.monotonic()+2;next_poll=time.monotonic()
            while time.monotonic()-started<600 and not done:
                now=time.monotonic()
                if not gate.verified and now>=next_identity:
                    if now-started>10:raise RuntimeError('Controller identity not verified')
                    send('api device');next_identity=now+2
                if gate.verified and now>=next_poll:
                    send('api tse');send('api status');next_poll=now+.25
                try:word=commands.get_nowait()
                except Empty:word=None
                if word=='quit':break
                if word:
                    if word not in ('arm','disarm'):print('Only arm | disarm | quit accepted.',flush=True)
                    elif word=='arm' and metadata['armSent']:print('One arm per fault-test session; restart console for another attempt.',flush=True)
                    else:
                        try:command,_=gate.command(word,now)
                        except ValueError as error:print('Refused:',error,flush=True)
                        else:
                            if word=='arm':metadata['armSent']=True
                            if word=='disarm':host_aborted=True
                            send(command)
                read_once()
                if fault_at and not metadata['stopProofVerified'] and time.monotonic()-fault_at>8:
                    raise RuntimeError('Radio stop not verified; switch MAIN POWER OFF')
                if restored_at and not metadata['recoveryNoRestartVerified'] and time.monotonic()-restored_at>15:
                    raise RuntimeError('Radio recovery/no-restart not verified')
        except KeyboardInterrupt:metadata['error']='Operator interrupted';exit_code=130
        except Exception as error:metadata['error']=str(error);print('Test stopped:',error,flush=True);exit_code=2
        finally:
            host_aborted=True
            if metadata['armSent'] and device.is_open:
                try:
                    send(CANCEL);end=time.monotonic()+3
                    while time.monotonic()<end and not cancel_ack:read_once()
                    stamp=time.monotonic();send('api tse');end=stamp+2
                    while time.monotonic()<end:
                        read_once()
                        ps,pulse=gate.latest.get('tse_pa25_pulse',(-100,{}))
                        if ps>=stamp:break
                    ps,pulse=gate.latest.get('tse_pa25_pulse',(-100,{}));ts,touch=gate.latest.get('tse_touch_test',(-100,{}))
                    no_actuation=ps>=stamp and ts>=stamp and pulse.get('writeAttempted') is False and pulse.get('startedAtMs')==baseline and touch.get('armed') is False and touch.get('runActive') is False
                    zero=ps>=stamp and restoration_confirmed(pulse)
                    metadata['cleanupVerified']=bool(cancel_ack and (zero or no_actuation))
                    if metadata['faultInjected'] and not metadata['radioRestoreSent'] and zero:
                        send(RADIO_ON);metadata['radioRestoreAfterCleanupSent']=True
                except Exception as error:metadata['cleanupError']=str(error)
                if not metadata.get('cleanupVerified',False):
                    print('Zero-state cleanup unconfirmed: switch MAIN POWER OFF.',flush=True);metadata['mainPowerOffRequired']=True
                    if exit_code==0:exit_code=2
            if device.is_open:device.close()
            metadata['finishedUtc']=utc_now();metadata['completed']=done
            metadata['result']='passed' if metadata['stopProofVerified'] and metadata['recoveryNoRestartVerified'] and metadata.get('cleanupVerified',False) else 'incomplete_or_inconclusive'
            if metadata['armSent'] and metadata['result']!='passed' and exit_code==0:exit_code=2
            (output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
            result=summarize(events);result['lastPulse']=next((e['object'] for e in reversed(events) if isinstance(e.get('object'),dict) and e['object'].get('type')=='tse_pa25_pulse'),None)
            (output/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
            print('USB closed; evidence saved.',flush=True)
    return exit_code

if __name__=='__main__':raise SystemExit(main())
