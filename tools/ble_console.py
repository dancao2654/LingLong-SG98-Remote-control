#!/usr/bin/env python3

import argparse
import asyncio
import json
from pathlib import Path
from typing import Optional

from bleak import BleakClient, BleakScanner


DEFAULT_NAME = "SG98 Console"
SERVICE_UUID = "6c660001-b5a3-4d3f-88f8-df8e8d1f9801"
TX_UUID = "6c660002-b5a3-4d3f-88f8-df8e8d1f9801"
STATE_PATH = Path.home() / ".sg98_ble_console.json"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="SG98 BLE error console")
    parser.add_argument("--address", help="BLE device address/UUID to connect to")
    parser.add_argument("--name", default=DEFAULT_NAME, help="Advertised BLE name to search for")
    parser.add_argument("--scan", action="store_true", help="List nearby BLE devices and exit")
    parser.add_argument("--timeout", type=float, default=5.0, help="Scan timeout in seconds")
    parser.add_argument(
        "--dump-timeout",
        type=float,
        default=20.0,
        help="Seconds to wait for the stored log dump after connect",
    )
    return parser


async def scan_devices(timeout: float) -> None:
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for device, adv in devices.values():
        names = [value for value in [adv.local_name, device.name] if value]
        name = " / ".join(dict.fromkeys(names)) or "-"
        marker = "*" if SERVICE_UUID.lower() in [uuid.lower() for uuid in adv.service_uuids] else " "
        print(f"{marker} {device.address}  {name}")


def load_cached_address() -> Optional[str]:
    if not STATE_PATH.exists():
        return None

    try:
        state = json.loads(STATE_PATH.read_text())
    except (OSError, json.JSONDecodeError):
        return None

    address = state.get("address")
    return address if isinstance(address, str) and address else None


def save_cached_address(address: str) -> None:
    try:
        STATE_PATH.write_text(json.dumps({"address": address}))
    except OSError:
        pass


def matches_console(device, adv, name: str) -> bool:
    if SERVICE_UUID.lower() in [uuid.lower() for uuid in adv.service_uuids]:
        return True

    name_lower = name.lower()
    for value in [adv.local_name, device.name]:
        if value and name_lower in value.lower():
            return True

    return False


async def find_target(address: Optional[str], name: str, timeout: float):
    if address:
        return address

    device = await BleakScanner.find_device_by_filter(
        lambda dev, adv: matches_console(dev, adv, name),
        timeout=timeout,
    )
    if device is not None:
        return device

    cached_address = load_cached_address()
    if cached_address:
        print(f'Using cached device address: {cached_address}')
        return cached_address

    raise RuntimeError(f'No BLE device matching "{name}" was found')


async def dump_console(client: BleakClient, timeout: float) -> None:
    rx_buffer = bytearray()
    dump_complete = asyncio.Event()
    saw_payload = False
    entry_count: Optional[int] = None

    def handle_notify(_: int, data: bytearray) -> None:
        nonlocal saw_payload, entry_count
        rx_buffer.extend(data)
        while b"\n" in rx_buffer:
            line, _, rest = rx_buffer.partition(b"\n")
            rx_buffer[:] = rest
            text = line.decode("utf-8", errors="replace")
            if text.startswith("__BEGIN__"):
                if "count=" in text:
                    try:
                        entry_count = int(text.split("count=", 1)[1].strip())
                    except ValueError:
                        entry_count = None
                return
            if text == "__END__":
                dump_complete.set()
                return
            saw_payload = True
            print(text)

    await client.start_notify(TX_UUID, handle_notify)
    print(f"Connected to {client.address}. Waiting for stored errors...")

    try:
        await asyncio.wait_for(dump_complete.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        raise RuntimeError("Timed out waiting for BLE error dump")
    finally:
        await client.stop_notify(TX_UUID)

    if entry_count is not None:
        print(f"entries: {entry_count}")
    if not saw_payload:
        print("no logged errors")


async def main() -> None:
    args = build_parser().parse_args()

    if args.scan:
        await scan_devices(args.timeout)
        return

    target = await find_target(args.address, args.name, args.timeout)
    async with BleakClient(target) as client:
        if not client.is_connected:
            raise RuntimeError("Failed to connect to BLE console")
        save_cached_address(client.address)
        await dump_console(client, args.dump_timeout)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print()
