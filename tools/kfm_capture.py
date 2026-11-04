#!/usr/bin/env python3
"""Read-only SG98 KFM USB capture. Never sends a motor or parameter command.

Use --port explicitly; USB port numbers can change after unplugging the board.
Firmware startup can itself send its configured initialisation commands. This
captures the USB console, not every packet on the physical RS485 bus.
"""
from __future__ import annotations
import argparse
import codecs
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import time

READ_ONLY_COMMANDS = ("api device", "api tse", "api status")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def normalize_mac(value: str) -> str:
    return value.replace(":", "").replace("-", "").lower()


def summarize(events: list[dict]) -> dict:
    objects = [e["object"] for e in events if isinstance(e.get("object"), dict)]
    statuses = [o for o in objects if o.get("type") == "status"]
    startup = [o for o in objects if o.get("type") == "tse_startup" and o.get("captured")]
    return {
        "device": next((o for o in objects if o.get("type") == "device"), None),
        "statusSamples": len(statuses),
        "observedErrors": sorted({o["error"] for o in statuses if isinstance(o.get("error"), int) and o["error"] != 0}),
        "lastStatus": statuses[-1] if statuses else None,
        "lastTse": next((o for o in reversed(objects) if o.get("type") == "tse"), None),
        "lastStartupSnapshot": startup[-1] if startup else None,
        "lastZeroProbe": next((o for o in reversed(objects) if o.get("type") == "tse_zero_probe"), None),
        "note": "USB observation only; no claim of physical stop, EEPROM persistence, or mains-cycle detection.",
    }


def capture(args) -> int:
    import serial
    output = args.output or Path("outputs") / ("kfm-capture-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    metadata = {"startedUtc": utc_now(), "port": args.port, "expectedMac": args.expected_mac,
                "commandsAllowed": READ_ONLY_COMMANDS, "durationSeconds": args.duration,
                "intervalSeconds": args.interval, "completed": False, "error": None}
    events: list[dict] = []
    kwargs = dict(port=None, baudrate=115200, timeout=0.1, rtscts=False, dsrdtr=False)
    if os.name != "nt": kwargs["exclusive"] = True
    device = serial.Serial(**kwargs)
    device.dtr = True
    device.rts = False
    device.port = args.port
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    pending_line = bytearray()
    identity_verified = False
    exit_code = 0
    try:
        with (output / "raw.bin").open("wb") as raw, (output / "serial.txt").open("w", encoding="utf-8") as readable, (output / "events.jsonl").open("w", encoding="utf-8") as journal:
            def record(direction: str, **fields):
                event = {"utc": utc_now(), "elapsedSeconds": round(time.monotonic() - started, 4), "direction": direction, **fields}
                events.append(event)
                journal.write(json.dumps(event, ensure_ascii=False) + "\n")
                journal.flush()

            def send(command: str):
                assert command in READ_ONLY_COMMANDS
                device.write((command + "\n").encode("ascii"))
                record("TX", command=command)

            device.open()
            print(f"Read-only USB capture: {args.port} -> {output.resolve()}")
            send("api device")
            next_identity = time.monotonic() + 2
            next_poll = time.monotonic()
            while time.monotonic() - started < args.duration:
                now = time.monotonic()
                if not identity_verified and now >= next_identity:
                    if now - started > 10: raise RuntimeError("No confirmed Waveshare controller identity within 10 seconds")
                    send("api device"); next_identity = now + 2
                if identity_verified and now >= next_poll:
                    send("api tse"); send("api status"); next_poll = now + args.interval
                chunk = device.read(8192)
                if not chunk: continue
                raw.write(chunk); raw.flush()
                readable.write(decoder.decode(chunk)); readable.flush()
                pending_line.extend(chunk)
                while b"\n" in pending_line:
                    line, _, rest = pending_line.partition(b"\n"); pending_line[:] = rest
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    try: obj = json.loads(text)
                    except ValueError: record("RX", text=text); continue
                    record("RX", object=obj)
                    if isinstance(obj, dict) and obj.get("type") == "device":
                        if obj.get("role") != "controller" or obj.get("target") != "waveshare_rs485_can":
                            raise RuntimeError("Connected device is not the Waveshare RS485/CAN controller")
                        if args.expected_mac and normalize_mac(obj.get("mac", "")) != normalize_mac(args.expected_mac):
                            raise RuntimeError("Connected controller MAC differs from --expected-mac")
                        identity_verified = True
                if len(pending_line) > 65536:
                    record("RX", discardedUnterminatedLineBytes=len(pending_line)); pending_line.clear()
            if not identity_verified: raise RuntimeError("No verified controller identity")
            readable.write(decoder.decode(b"", final=True))
            metadata["completed"] = True
    except KeyboardInterrupt:
        metadata["error"] = "Interrupted by operator; no control commands sent"
        exit_code = 130
    except Exception as error:
        metadata["error"] = str(error)
        print(f"Capture stopped: {error}")
        exit_code = 2
    finally:
        if device.is_open: device.close()
        metadata["finishedUtc"] = utc_now()
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        summary = summarize(events)
        (output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
        print(f"Saved {summary['statusSamples']} status samples; USB port closed.")
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("ports", help="List port metadata without opening a device")
    cap = commands.add_parser("capture", help="Only api device / api tse / api status")
    cap.add_argument("--port", required=True)
    cap.add_argument("--expected-mac")
    cap.add_argument("--duration", type=float, default=60)
    cap.add_argument("--interval", type=float, default=2)
    cap.add_argument("--output", type=Path)
    summary = commands.add_parser("summarize", help="Summarize an existing capture offline")
    summary.add_argument("events", type=Path)
    args = parser.parse_args()
    if args.command == "ports":
        from serial.tools import list_ports
        for item in list_ports.comports(): print(f"{item.device}\t{item.description}\t{item.hwid}")
        return 0
    if args.command == "summarize":
        events = [json.loads(line) for line in args.events.read_text().splitlines() if line.strip()]
        print(json.dumps(summarize(events), indent=2, ensure_ascii=False)); return 0
    if not 1 <= args.duration <= 7200 or not 0.5 <= args.interval <= 60:
        parser.error("duration must be 1..7200 seconds; interval must be 0.5..60 seconds")
    return capture(args)


if __name__ == "__main__":
    raise SystemExit(main())
