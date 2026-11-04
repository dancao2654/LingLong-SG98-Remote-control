#!/usr/bin/env python3
"""Operator-present console for the dedicated KFM supervised fixed-speed PA25 pulse firmware.

Explicitly enables at checked zero, then requests fixed100rpm for nominal2seconds.
No UI starts, variable speed, other presets or persistent writes are allowed.
Does not flash firmware. Keep the empty grinder closed and main switch reachable.
A USB fault can prevent cancellation: the main switch remains the independent stop.
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
    from .kfm_capture import utc_now, normalize_mac, summarize, READ_ONLY_COMMANDS
except ImportError:
    from kfm_capture import utc_now, normalize_mac, summarize, READ_ONLY_COMMANDS

START = "api tse pa25_pulse start"
CANCEL = "api tse pa25_pulse cancel"
RESTORE = "api tse pa25_pulse restore"
ALLOWED_WIRE_COMMANDS = (*READ_ONLY_COMMANDS, START, CANCEL, RESTORE)


def restoration_confirmed(sample) -> bool:
    return (isinstance(sample, dict)
            and sample.get("type") == "tse_pa25_pulse"
            and sample.get("available") is True
            and sample.get("restoreAcknowledged") is True
            and sample.get("restoreReadbackValid") is True
            and sample.get("latestValid") is True)


class OperatorGate:
    def __init__(self, mac: str, version: str = "2026.10.27.7"):
        self.mac, self.version = mac, version
        self.verified = False
        self.latest: dict[str, tuple[float, dict]] = {}

    def observe(self, obj, now: float):
        if not isinstance(obj, dict): return
        kind = obj.get("type")
        if kind == "device":
            self.verified = False
            self.latest.clear()
            if (obj.get("role") != "controller" or obj.get("target") != "waveshare_rs485_can"
                or obj.get("buildEnv") != "controller-kfm-pa25-pulse"
                or obj.get("firmwareVersion") != self.version
                or normalize_mac(obj.get("mac", "")) != normalize_mac(self.mac)):
                raise RuntimeError("Identity/firmware mismatch; no probe start permitted")
            self.verified = True
        if isinstance(kind, str): self.latest[kind] = (now, obj)

    def command(self, word: str, now: float) -> tuple[str | None, str | None]:
        if word in ("off", "on"): return None, "operator_confirmed_main_switch_" + word
        if word == "quit": return None, "quit"
        if word not in ("pulse", "cancel", "restore"):
            raise ValueError("Allowed input: pulse, cancel, restore, off, on, quit")
        if not self.verified: raise ValueError("Controller identity is not confirmed")
        if word == "cancel": return CANCEL, None
        if word == "restore": return RESTORE, None
        samples = {}
        for kind in ("status", "tse", "tse_pa25_pulse"):
            stamp, sample = self.latest.get(kind, (-100.0, {}))
            if now - stamp > 3: raise ValueError("Fresh status/diagnostic samples are required")
            samples[kind] = sample
        s, d, z = (samples[k] for k in ("status", "tse", "tse_pa25_pulse"))
        if (s.get("state") != "IDLE" or s.get("comm") != "CONNECTED" or s.get("error") != 0
            or s.get("currentRPM") != 0 or s.get("motorSetRPM") != 0
            or d.get("phase") != "complete" or d.get("kfm") is not True
            or d.get("identified") is not True or d.get("motionArmed") is not False
            or d.get("monitorValid") is not True or d.get("alarmValid") is not True
            or z.get("available") is not True
            or z.get("phase") not in ("idle", "complete", "cancelled", "failed")):
            raise ValueError("Controller is not in the required stationary, locked KFM state")
        return START, None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--expected-version", default="2026.10.27.7")
    parser.add_argument("--operator-present", action="store_true", help="Required acknowledgement: operator present, grinder empty/closed, main switch reachable")
    parser.add_argument("--minutes", type=float, default=60)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.operator_present: parser.error("--operator-present is required; no unattended motor pulse")
    if not 1 <= args.minutes <= 60: parser.error("minutes must be 1..60")
    import serial
    output = args.output or Path("outputs") / ("kfm-pa25-pulse-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))
    output.mkdir(parents=True, exist_ok=False)
    gate = OperatorGate(args.expected_mac, args.expected_version)
    commands: Queue = Queue()
    def stdin_reader():
        for line in sys.stdin: commands.put(line.strip())
        commands.put("quit")
    threading.Thread(target=stdin_reader, daemon=True).start()
    started = time.monotonic(); events = []; partial = bytearray()
    serial_options = dict(port=None, baudrate=115200, timeout=.1)
    if os.name != "nt": serial_options["exclusive"] = True
    device = serial.Serial(**serial_options)
    device.dtr = True; device.rts = False; device.port = args.port
    probe_start_attempted = False
    cancel_acknowledged = False
    metadata = {"startedUtc": utc_now(), "port": args.port, "expectedMac": args.expected_mac,
                "expectedVersion": args.expected_version, "operatorPresentAcknowledged": True,
                "commandsAllowed": ALLOWED_WIRE_COMMANDS, "error": None}
    exit_code = 0
    with (output/"raw.bin").open("wb") as raw, (output/"events.jsonl").open("w") as journal:
        def record(direction, **data):
            event = {"utc": utc_now(), "elapsedSeconds": round(time.monotonic()-started, 4), "direction": direction, **data}
            events.append(event); journal.write(json.dumps(event, ensure_ascii=False)+"\n"); journal.flush()
        def send(command):
            nonlocal cancel_acknowledged
            assert command in ALLOWED_WIRE_COMMANDS
            if command == CANCEL: cancel_acknowledged = False
            device.write((command+"\n").encode("ascii")); record("TX", command=command)
        last_phase = None
        last_capture = None
        def read_once():
            nonlocal last_phase, last_capture, cancel_acknowledged
            chunk=device.read(8192)
            if not chunk: return
            raw.write(chunk); raw.flush(); partial.extend(chunk)
            while b"\n" in partial:
                line, _, rest=partial.partition(b"\n"); partial[:]=rest
                text=line.decode("utf-8","replace").rstrip("\r")
                try: obj=json.loads(text)
                except ValueError: record("RX", text=text); continue
                record("RX", object=obj); gate.observe(obj,time.monotonic())
                if isinstance(obj,dict) and obj.get("cmd")==CANCEL.removeprefix("api ") and obj.get("ok") is True:
                    cancel_acknowledged=True
                if isinstance(obj,dict) and obj.get("type")=="tse_pa25_pulse":
                    state=(obj.get("phase"),obj.get("reason"))
                    if state!=last_phase:
                        print("Probe:",state[0],"/",state[1],flush=True); last_phase=state
                    if obj.get("afterGapCaptured"):
                        captured=(obj.get("afterPa25"),obj.get("settledPa25"),obj.get("presetConsistent"),obj.get("restoreAcknowledged"))
                        if captured!=last_capture:
                            print("Observed PA25 first/settled/consistent/restoreAck:",captured,flush=True)
                            last_capture=captured
            if len(partial)>65536: record("RX", discardedUnterminatedLineBytes=len(partial)); partial.clear()
        try:
            device.open()
            print("PA25 PULSE console. Explicit pulse enables and requests100rpm for nominal2seconds. MAIN SWITCH REQUIRED.",flush=True)
            print("Input: pulse | cancel | restore | off | on | quit. off/on only journal your physical switch action.",flush=True)
            print("Log:",output.resolve(),flush=True)
            send("api device"); next_identity=time.monotonic()+2; next_poll=time.monotonic()
            while time.monotonic()-started < args.minutes*60:
                now=time.monotonic()
                if not gate.verified and now>=next_identity:
                    if now-started>10: raise RuntimeError("Controller identity not confirmed within 10 seconds")
                    send("api device"); next_identity=now+2
                if gate.verified and now>=next_poll:
                    send("api tse"); send("api status"); next_poll=now+1
                try: word=commands.get_nowait()
                except Empty: word=None
                if word:
                    try: command,event=gate.command(word,now)
                    except ValueError as error: print("Refused:",error,flush=True)
                    else:
                        if event: record("OPERATOR", event=event)
                        if event=="quit": break
                        if command:
                            if command in (START, RESTORE): probe_start_attempted=True
                            send(command)
                read_once()
        except KeyboardInterrupt:
            metadata["error"]="Interrupted by operator"; exit_code=130
        except Exception as error:
            metadata["error"]=str(error); print("Session stopped:",error,flush=True); exit_code=2
        finally:
            if probe_start_attempted and device.is_open:
                try:
                    send(CANCEL); end=time.monotonic()+3
                    while time.monotonic()<end and not cancel_acknowledged: read_once()
                    verification_started=time.monotonic()
                    send("api tse")
                    end=time.monotonic()+2
                    while time.monotonic()<end:
                        read_once()
                        stamp,sample=gate.latest.get("tse_pa25_pulse",(-1,{}))
                        if stamp>=verification_started: break
                    stamp,sample=gate.latest.get("tse_pa25_pulse",(-1,{}))
                    metadata["restorationReadbackConfirmed"]=stamp>=verification_started and restoration_confirmed(sample)
                except Exception as error: record("HOST", cancelError=str(error))
                if not cancel_acknowledged or not metadata.get("restorationReadbackConfirmed",False):
                    print("Cancellation or zero-state readback unconfirmed. Switch MAIN POWER OFF.",flush=True)
                    metadata["cancellationUnconfirmed"]=True
                    if exit_code==0: exit_code=2
            if device.is_open: device.close()
            metadata["finishedUtc"]=utc_now();metadata["probeStartAttempted"]=probe_start_attempted
            (output/"metadata.json").write_text(json.dumps(metadata,indent=2)+"\n")
            summary=summarize(events)
            summary["lastPulse"]=next((e["object"] for e in reversed(events) if isinstance(e.get("object"),dict) and e["object"].get("type")=="tse_pa25_pulse"),None)
            (output/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
            print("USB port closed; session saved.",flush=True)
    return exit_code


if __name__=="__main__": raise SystemExit(main())
