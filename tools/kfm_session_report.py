#!/usr/bin/env python3
"""Offline CSV/Markdown report from existing USB events. Never opens a device."""
from __future__ import annotations
import argparse
import csv
import json
from pathlib import Path

FIELDS=('utc','elapsedSeconds','state','requestedRPM','actualRPM','storedRPM','comm','error','backendReason','armed','remoteFresh','stopReadbackVerified')

def analyze(events):
    latest={};rows=[];changes=[];errors=set();devices=[];invalid=0
    for event in events:
        if not isinstance(event,dict):invalid+=1;continue
        obj=event.get('object')
        if not isinstance(obj,dict):continue
        kind=obj.get('type');stamp=event.get('elapsedSeconds')
        if not isinstance(stamp,(int,float)):invalid+=1;continue
        if kind=='device':
            # Repeated identity reports may follow a reboot; never carry an old
            # zero/arm proof across that boundary.
            devices.append(obj);latest.clear()
        previous=latest.get(kind,(None,{}))[1] if isinstance(kind,str) else {}
        if isinstance(kind,str):latest[kind]=(stamp,obj)
        if kind in ('tse','tse_touch_test','tse_pa25_pulse') and obj.get('reason')!=previous.get('reason'):
            changes.append({'utc':event.get('utc'),'elapsedSeconds':stamp,'source':kind,'reason':obj.get('reason'),'phase':obj.get('phase')})
        if kind!='status':continue
        def fresh(name):
            at,value=latest.get(name,(-1e20,{}))
            return value if 0<=stamp-at<=3 else {}
        backend=fresh('tse');candidate=fresh('tse_run_candidate');touch=fresh('tse_touch_test')
        error=obj.get('error')
        if isinstance(error,int) and error!=0:errors.add(error)
        # A firmware readback flag is useful evidence, never a physical stop
        # guarantee. Require a matching current connected zero-speed sample.
        proof=candidate.get('stopReadbackVerified') if candidate.get('available') is True else None
        if proof is True:
            proof=(obj.get('comm')=='CONNECTED' and obj.get('currentRPM')==0 and obj.get('motorSetRPM')==0)
        row={'utc':event.get('utc'),'elapsedSeconds':stamp,'state':obj.get('state'),
             'requestedRPM':obj.get('motorSetRPM'),'actualRPM':obj.get('currentRPM'),
             'storedRPM':obj.get('setRPM'),'comm':obj.get('comm'),'error':error,
             'backendReason':backend.get('reason'),'armed':backend.get('motionArmed'),
             'remoteFresh':candidate.get('remoteFresh') if candidate.get('available') is True else touch.get('remoteFresh') if touch.get('available') is True else None,'stopReadbackVerified':proof}
        rows.append(row)
    peaks=[abs(row['actualRPM']) for row in rows if isinstance(row['actualRPM'],(int,float))]
    return {'devices':devices,'statusSamples':len(rows),'reportedPeakAbsRPM':max(peaks) if peaks else None,
            'observedErrors':sorted(errors),'reasonChanges':changes,'rows':rows,'ignoredMalformedRecords':invalid,
            'scope':'USB observations only. No physical stop, exact fault latency, EEPROM persistence or drive-side watchdog is inferred.'}

def load_events(path):
    result=[]
    with path.open(encoding='utf-8') as handle:
        for line in handle:
            if not line.strip():continue
            try:result.append(json.loads(line))
            except (ValueError,TypeError):result.append(None)
    return result

def safe_cell(value):
    # Serial strings are untrusted. Keep CSV formulas inert, retaining numeric
    # negative RPM values as numbers.
    if isinstance(value,str) and value.lstrip().startswith(('=','+','-','@')):return "'"+value
    return '' if value is None else value

def markdown_cell(value):
    return str(value if value is not None else 'unknown').replace('|','\\|').replace('\n',' ')

def write_report(events,output):
    result=analyze(events);output.mkdir(parents=True,exist_ok=False)
    with (output/'timeline.csv').open('w',newline='',encoding='utf-8') as handle:
        writer=csv.DictWriter(handle,fieldnames=FIELDS);writer.writeheader()
        for row in result['rows']:writer.writerow({key:safe_cell(value) for key,value in row.items()})
    (output/'analysis.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
    text=['# SG98 USB session report','',f"Status samples: {result['statusSamples']}",
          f"Peak reported |RPM|: {result['reportedPeakAbsRPM']}",f"Observed error codes: {result['observedErrors']}",
          f"Malformed records skipped: {result['ignoredMalformedRecords']}",'',result['scope'],'',
          'Blank CSV fields mean unavailable/stale data. Diagnostics older than three seconds are excluded from status rows. A true stopReadbackVerified field is the firmware/register observation paired with a connected zero-speed USB status; it is not independent mechanical proof.',
          '', '## Reported reason changes','', '| Time (s) | Source | Phase | Reported reason |','| --- | --- | --- | --- |']
    changes=result['reasonChanges']
    selected=changes if len(changes)<=100 else changes[:50]+changes[-50:]
    for row in selected:text.append('| '+' | '.join(markdown_cell(row.get(key)) for key in ('elapsedSeconds','source','phase','reason'))+' |')
    if len(changes)>100:text+=['',f"Showing first/last100 of {len(changes)} changes. Complete records are in analysis.json."]
    text+=['','The reason order is retained, including any later disarm/cleanup reason. Do not replace an earlier timeout or radio-loss cause with the final cleanup label. This report does not label a test passed automatically.','']
    (output/'REPORT_EN.md').write_text('\n'.join(text))
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path,help='events.jsonl or its session directory')
    parser.add_argument('--output',type=Path,required=True,help='New report directory; existing files are never overwritten')
    args=parser.parse_args();source=args.input/'events.jsonl' if args.input.is_dir() else args.input
    result=write_report(load_events(source),args.output)
    print(f"Saved {result['statusSamples']} status rows to {args.output.resolve()}; no device accessed.")
    return 0
if __name__=='__main__':raise SystemExit(main())
