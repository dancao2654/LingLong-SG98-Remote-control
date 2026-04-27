#!/usr/bin/env python3
import argparse
import glob
import json
import math
import sys
import time
from collections import deque

import serial


# Current Mac bench wiring:
# remote: /dev/cu.usbmodem21201, USB serial 1C:DB:D4:7B:67:94
# controller: /dev/cu.usbmodem21101, USB serial FC:01:2C:D6:38:BC
DEFAULT_REMOTE_PORT = "auto"
DEFAULT_CONTROLLER_PORT = "auto"
DEFAULT_CONTROLLER_MAC = "auto"
FALLBACK_CONTROLLER_MAC = "FC:01:2C:D6:38:BC"


class TestFailure(RuntimeError):
    pass


class SerialConsole:
    def __init__(self, label, port, baudrate=115200, timeout=0.2):
        self.label = label
        self.port = port
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.log = deque(maxlen=200)
        self.timings = []

    def close(self):
        self.ser.close()

    def _record(self, line):
        self.log.append(line)

    def recent_log(self):
        return list(self.log)

    def send(self, command):
        self.ser.write((command + "\n").encode("utf-8"))
        self.ser.flush()

    def drain(self, seconds=0.2):
        end = time.time() + seconds
        while time.time() < end:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                self._record(line)

    def read_line(self, timeout=1.0):
        end = time.time() + timeout
        while time.time() < end:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                self._record(line)
                return line
        return None

    def request_json(self, command, type_name, timeout=8.0):
        self.drain(0.1)
        start = time.perf_counter()
        self.send(command)
        end = time.time() + timeout
        while time.time() < end:
            line = self.read_line(timeout=0.5)
            if line is None:
                continue
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if payload.get("type") == type_name:
                self.timings.append((command, (time.perf_counter() - start) * 1000.0))
                return payload
        raise TestFailure(f"{self.label}: timeout waiting for {type_name} after `{command}`")

    def request_controller_snapshot(self, timeout=10.0):
        self.drain(0.1)
        start = time.perf_counter()
        self.send("api snapshot")
        end = time.time() + timeout
        snapshot = {"menus": {}}
        in_snapshot = False
        while time.time() < end:
            line = self.read_line(timeout=0.5)
            if line is None:
                continue
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            msg_type = payload.get("type")
            if msg_type == "snapshot_begin":
                in_snapshot = True
                continue
            if msg_type == "snapshot_end":
                self.timings.append(("api snapshot", (time.perf_counter() - start) * 1000.0))
                return snapshot
            if not in_snapshot:
                continue
            if msg_type == "status":
                snapshot["status"] = payload
            elif msg_type == "menu":
                snapshot["menus"][payload["menu"]] = {
                    "meta": payload,
                    "items": {},
                }
            elif msg_type == "item":
                menu_num = payload["menu"]
                snapshot["menus"].setdefault(menu_num, {"meta": {"menu": menu_num}, "items": {}})
                snapshot["menus"][menu_num]["items"][payload["index"]] = payload
        self.timings.append(("api snapshot", (time.perf_counter() - start) * 1000.0))
        raise TestFailure(f"{self.label}: timeout waiting for controller snapshot")

    def timing_summary(self):
        values = sorted(duration for _, duration in self.timings)
        if not values:
            return {"count": 0, "avg_ms": 0.0, "p95_ms": 0.0, "max_ms": 0.0}
        p95_index = min(len(values) - 1, max(0, math.ceil(len(values) * 0.95) - 1))
        return {
            "count": len(values),
            "avg_ms": sum(values) / len(values),
            "p95_ms": values[p95_index],
            "max_ms": values[-1],
        }


class DualBoardRegression:
    def __init__(self, remote_port, controller_port, controller_mac, skip_websocket=False, factory_network_reset=False):
        self.remote = SerialConsole("remote", remote_port)
        self.controller = SerialConsole("controller", controller_port)
        self.controller_mac = controller_mac
        self.original = {}
        self.skip_websocket = skip_websocket
        self.factory_network_reset = factory_network_reset

    def close(self):
        self.remote.close()
        self.controller.close()

    def fail(self, message):
        details = [
            message,
            "",
            "Recent remote log:",
            *self.remote.recent_log()[-30:],
            "",
            "Recent controller log:",
            *self.controller.recent_log()[-30:],
        ]
        raise TestFailure("\n".join(details))

    def expect(self, condition, message):
        if not condition:
            self.fail(message)

    def remote_status(self):
        return self.remote.request_json("api status", "status")["data"]

    def remote_snapshot(self):
        return self.remote.request_json("api snapshot", "snapshot")["data"]

    def controller_status(self):
        return self.controller.request_json("api status", "status")

    def controller_snapshot(self):
        return self.controller.request_controller_snapshot()

    def controller_menu_value(self, snapshot, menu_num, item_idx):
        return snapshot["menus"][menu_num]["items"][item_idx]["value"]

    def remote_menu_value(self, snapshot, menu_num, item_idx):
        for menu in snapshot["menus"]:
            if menu["menu"] == menu_num:
                return menu["values"][item_idx]
        self.fail(f"remote snapshot missing menu {menu_num}")

    def wait_until(self, description, fn, timeout=15.0, interval=0.4):
        end = time.time() + timeout
        last_error = None
        while time.time() < end:
            try:
                value = fn()
                if value:
                    return value
            except Exception as exc:
                last_error = exc
            time.sleep(interval)
        if last_error is not None:
            self.fail(f"{description} failed: {last_error}")
        self.fail(f"timeout waiting for {description}")

    def controller_command_ok(self, command, ack_cmd=None):
        expected_cmd = ack_cmd or command.split()[1]
        payload = self.controller.request_json(command, "ack")
        self.expect(payload.get("cmd") == expected_cmd and payload.get("ok") is True,
                    f"controller command failed: {command} -> {payload}")
        return payload

    def controller_command_expect_fail(self, command, ack_cmd=None):
        expected_cmd = ack_cmd or command.split()[1]
        payload = self.controller.request_json(command, "ack")
        self.expect(payload.get("cmd") == expected_cmd and payload.get("ok") is False,
                    f"controller command unexpectedly succeeded: {command} -> {payload}")
        return payload

    def remote_command_ok(self, command, expected_cmd=None):
        payload = self.remote.request_json(command, "ack")
        want = expected_cmd or command
        self.expect(payload.get("cmd") == want and payload.get("ok") is True,
                    f"remote command failed: {command} -> {payload}")
        return payload

    def capture_original_state(self):
        snap = self.controller_snapshot()
        status = snap["status"]
        self.original = {
            "radio_mode": self.controller_menu_value(snap, 1, 4),
            "sleep_time": self.controller_menu_value(snap, 1, 5),
            "purge_auto": self.controller_menu_value(snap, 2, 1),
            "gbw_rpm": self.controller_menu_value(snap, 3, 1),
            "rpm": status["setRPM"],
            "weight": status["setWeight"],
            "state": status["state"],
        }

    def restore_original_state(self):
        if not self.original:
            return
        self.controller_command_ok(f"api set 1 5 {self.original['sleep_time']}")
        self.controller_command_ok(f"api set 2 1 {self.original['purge_auto']}")
        self.controller_command_ok(f"api set 3 1 {self.original['gbw_rpm']}")
        self.controller_command_ok(f"api setrpm {self.original['rpm']}", ack_cmd="setrpm")
        self.controller_command_ok(f"api setweight {self.original['weight']}", ack_cmd="setweight")
        if self.original["state"] == "IDLE_GBW":
            self.controller_command_ok("api action idle_gbw", ack_cmd="action")
        else:
            self.controller_command_ok("api action idle", ack_cmd="action")
        self.controller_command_ok(f"api set 1 4 {self.original['radio_mode']}")
        if self.original["radio_mode"] == 0:
            self.remote_command_ok("transport ws")
        else:
            self.remote_command_ok("peer lock " + self.controller_mac)
            self.remote_command_ok("transport espnow")

    def reset_network_for_factory_test(self):
        print("== Factory/no-info reset ==")
        self.remote_command_ok("api action reset_network")
        self.remote_command_ok("peer clear")
        self.controller_command_ok("api action reset_network", ack_cmd="action")
        self.controller_command_ok("api action idle", ack_cmd="action")
        self.controller_command_ok("api set 1 4 1")
        self.remote_command_ok("transport espnow")

    def fresh_discovery_suite(self):
        print("== ESP-NOW-first no-info discovery suite ==")
        self.controller_command_ok("api set 1 4 1")
        self.remote_command_ok("api action reset_network")
        self.remote_command_ok("peer clear")
        self.remote_command_ok("transport espnow")
        status = self.wait_remote_transport("ESP-NOW", require_online=True, timeout=35.0)
        self.expect(status["lockedPeerConfigured"] is True and status["lockedPeer"],
                    f"fresh ESP-NOW discovery did not lock a controller peer: {status}")
        self.expect(status["wifi"]["staSaved"] is False,
                    f"fresh remote should not have saved Wi-Fi during no-info test: {status}")
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "ESP-NOW",
                    f"fresh controller should prefer ESP-NOW-only after link: {controller_status}")
        self.expect(status["wifi"]["active"] is False,
                    f"remote Wi-Fi should be off once ESP-NOW is active: {status}")
        self.load_probe("Fresh ESP-NOW discovery")

    def web_mode_matrix_suite(self):
        print("== Web mode matrix suite ==")
        self.controller_command_ok("api action reset_network", ack_cmd="action")

        controller_status = self.controller_status()
        if controller_status.get("remoteRecoveryRequired"):
            payload = self.controller_command_expect_fail("api set 1 4 0", ack_cmd="set")
            self.expect("web-only" in payload.get("message", "").lower(),
                        f"headless WEB-only rejection message was unexpected: {payload}")
            controller_status = self.controller_status()
            self.expect(controller_status["radioMode"] != "WEB",
                        f"headless controller must not enter WEB-only mode: {controller_status}")
        else:
            self.controller_command_ok("api set 1 4 0")
            controller_status = self.controller_status()
            self.expect(controller_status["radioMode"] == "WEB" and controller_status["radioStatus"] == "AP only",
                        f"plain WEB mode should be AP/Wi-Fi only, not dual: {controller_status}")
            self.remote_command_ok("peer clear")
            self.remote_command_ok("transport espnow")
            time.sleep(6.0)
            remote_status = self.remote_status()
            self.expect(remote_status["transport"] == "ESP-NOW" and not remote_status["linkOnline"],
                        f"plain WEB-only should not pretend ESP-NOW is reachable: {remote_status}")

        self.controller_command_ok("api set 1 4 3")
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "WEB+ESP",
                    f"controller should enter WEB+ESP for web GUI/recovery: {controller_status}")
        self.remote_command_ok("transport espnow")
        self.remote_command_ok("peer scan")
        remote_status = self.wait_remote_transport("ESP-NOW", require_online=True, timeout=35.0)
        time.sleep(6.0)
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "WEB+ESP" and "AP only" in controller_status["radioStatus"],
                    f"WEB+ESP should keep the web GUI/recovery AP available while ESP-NOW is online: {controller_status}")
        remote_status = self.remote_status()
        self.expect(remote_status["wifi"]["active"] is False,
                    f"remote Wi-Fi should stay off while using controller WEB+ESP over ESP-NOW: {remote_status}")
        self.load_probe("WEB+ESP web GUI / recovery")

        self.controller_command_ok("api set 1 4 1")
        remote_status = self.wait_remote_transport("ESP-NOW", require_online=True, timeout=15.0)
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "ESP-NOW",
                    f"explicit ESP-NOW mode should turn controller Wi-Fi/AP off: {controller_status}")
        self.expect(remote_status["wifi"]["active"] is False,
                    f"remote Wi-Fi should remain off in explicit ESP-NOW mode: {remote_status}")

    def full_size_controller_only_suite(self):
        status = self.controller_status()
        if status.get("remoteRecoveryRequired"):
            print("== Full-size controller-only suite skipped: headless recovery profile active ==")
            return

        print("== Full-size controller-only suite ==")
        self.controller_command_ok("api set 1 4 1")
        self.remote_command_ok("transport ws")
        time.sleep(8.0)
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "ESP-NOW" and not controller_status["remoteRecoveryRequired"],
                    f"full-size controller-only should remain locally usable in ESP-NOW mode: {controller_status}")

        self.controller_command_ok("api set 1 4 3")
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "WEB+ESP",
                    f"full-size controller-only should allow local WEB+ESP for phone UI: {controller_status}")

        self.controller_command_ok("api set 1 4 2")
        controller_status = self.controller_status()
        self.expect(controller_status["radioMode"] == "OFF",
                    f"full-size controller-only should allow OFF because local recovery exists: {controller_status}")

        self.controller_command_ok("api set 1 4 1")
        self.remote_command_ok("transport espnow")
        self.remote_command_ok("peer scan")
        self.wait_remote_transport("ESP-NOW", require_online=True, timeout=35.0)

    def headless_espnow_fallback_suite(self):
        status = self.controller_status()
        if not status.get("remoteRecoveryRequired"):
            print("== Headless ESP-NOW fallback skipped: full-size local UI available ==")
            return

        print("== Headless ESP-NOW fallback suite ==")
        self.controller_command_ok("api set 1 4 3")
        self.controller_command_ok("api set 1 4 1")
        self.remote_command_ok("transport ws")
        recovered = self.wait_until(
            "headless ESP-NOW boot fallback to WEB+ESP",
            lambda: (lambda s: s if s["radioMode"] == "WEB+ESP" and s.get("radioRecovery") else None)(self.controller_status()),
            timeout=75.0,
            interval=1.0,
        )
        self.expect(recovered["radioMode"] == "WEB+ESP",
                    f"headless controller did not fall back to WEB+ESP recovery: {recovered}")
        self.remote_command_ok("transport espnow")
        self.remote_command_ok("peer scan")
        self.wait_remote_transport("ESP-NOW", require_online=True, timeout=35.0)
        self.load_probe("Headless ESP-NOW fallback")

    def wait_remote_transport(self, expected_transport, require_online=False, timeout=20.0):
        def check():
            status = self.remote_status()
            if status["transport"] != expected_transport:
                return None
            if require_online and not status["linkOnline"]:
                return None
            return status
        return self.wait_until(f"remote transport {expected_transport}", check, timeout=timeout)

    def wait_controller_rpm(self, rpm, timeout=12.0):
        def check():
            status = self.controller_status()
            return status if status["setRPM"] == rpm else None

        return self.wait_until(
            f"controller rpm {rpm}",
            check,
            timeout=timeout,
        )

    def wait_controller_weight(self, weight, timeout=12.0):
        def check():
            status = self.controller_status()
            return status if status["setWeight"] == weight else None

        return self.wait_until(
            f"controller weight {weight}",
            check,
            timeout=timeout,
        )

    def wait_controller_state(self, state, timeout=12.0):
        def check():
            status = self.controller_status()
            return status if status["state"] == state else None

        return self.wait_until(
            f"controller state {state}",
            check,
            timeout=timeout,
        )

    def wait_remote_status_field(self, path, expected, timeout=12.0):
        keys = path.split(".")

        def check():
            status = self.remote_status()
            value = status
            for key in keys:
                value = value[key]
            return status if value == expected else None

        return self.wait_until(f"remote {path} == {expected}", check, timeout=timeout)

    def wait_controller_menu(self, menu_num, item_idx, value, timeout=12.0):
        def check():
            snap = self.controller_snapshot()
            return snap if self.controller_menu_value(snap, menu_num, item_idx) == value else None

        return self.wait_until(f"controller menu {menu_num}/{item_idx} == {value}", check, timeout=timeout)

    def wait_remote_menu(self, menu_num, item_idx, value, timeout=12.0):
        def check():
            snap = self.remote_snapshot()
            return snap if self.remote_menu_value(snap, menu_num, item_idx) == value else None

        return self.wait_until(f"remote menu {menu_num}/{item_idx} == {value}", check, timeout=timeout)

    def espnow_suite(self):
        print("== ESP-NOW suite ==")
        self.controller_command_ok("api set 1 4 1")
        self.remote_command_ok("peer lock " + self.controller_mac)
        self.remote_command_ok("transport espnow")
        self.wait_remote_transport("ESP-NOW", require_online=True)
        self.remote_command_ok("raw GET_MENU=4")
        self.wait_remote_menu(4, 5, 0)

        self.controller_command_ok("api setrpm 650", ack_cmd="setrpm")
        self.wait_remote_status_field("controller.setRPM", 650)
        self.wait_remote_status_field("manualRPM", 650)

        self.remote_command_ok("api setrpm 775")
        self.wait_controller_rpm(775)
        self.wait_remote_status_field("controller.setRPM", 775)

        self.controller_command_ok("api setweight 19000", ack_cmd="setweight")
        self.wait_remote_status_field("controller.setWeightMg", 19000)
        self.wait_remote_status_field("weightMg", 19000)

        self.remote_command_ok("api setweight 21000")
        self.wait_controller_weight(21000)
        self.wait_remote_status_field("controller.setWeightMg", 21000)

        new_sleep = 125 if self.original["sleep_time"] != 125 else 120
        self.remote_command_ok(f"api set 1 5 {new_sleep}")
        self.wait_controller_menu(1, 5, new_sleep)
        self.wait_remote_menu(1, 5, new_sleep)

        new_purge_auto = 0 if self.original["purge_auto"] else 1
        self.remote_command_ok(f"api set 2 1 {new_purge_auto}")
        self.wait_controller_menu(2, 1, new_purge_auto)
        self.wait_remote_menu(2, 1, new_purge_auto)

        new_gbw_rpm = 325 if self.original["gbw_rpm"] != 325 else 350
        self.remote_command_ok(f"api set 3 1 {new_gbw_rpm}")
        self.wait_controller_menu(3, 1, new_gbw_rpm)
        self.wait_remote_menu(3, 1, new_gbw_rpm)

        self.controller_command_ok("api set 2 1 " + str(self.original["purge_auto"]))
        self.wait_remote_menu(2, 1, self.original["purge_auto"])

        self.remote_command_ok("api action idle_gbw")
        self.wait_controller_state("IDLE_GBW")
        self.wait_remote_status_field("controller.state", "IDLE_GBW")

        self.remote_command_ok("api action idle")
        self.wait_controller_state("IDLE")
        self.wait_remote_status_field("controller.state", "IDLE")

        for index in range(10):
            rpm = 700 + (index * 25)
            weight = 18000 + (index * 250)
            self.remote_command_ok(f"api setrpm {rpm}")
            self.wait_controller_rpm(rpm)
            self.controller_command_ok(f"api setweight {weight}", ack_cmd="setweight")
            self.wait_remote_status_field("controller.setWeightMg", weight)
            status = self.remote_status()
            self.expect(status["transport"] == "ESP-NOW" and status["linkOnline"],
                        f"ESP-NOW soak lost link on iteration {index}: {status}")
        self.load_probe("ESP-NOW")

    def websocket_suite(self):
        print("== WebSocket suite ==")
        self.controller_command_ok("api set 1 4 0")
        self.remote_command_ok("transport ws")
        self.wait_remote_transport("WebSocket", require_online=True, timeout=40.0)
        self.wait_remote_status_field("wsConnected", True, timeout=20.0)
        self.wait_remote_menu(1, 4, 0, timeout=20.0)
        self.remote_command_ok("raw GET_MENU=4")
        self.wait_remote_menu(4, 5, 0, timeout=20.0)

        self.controller_command_ok("api setrpm 825", ack_cmd="setrpm")
        self.wait_remote_status_field("controller.setRPM", 825, timeout=20.0)

        self.remote_command_ok("api setrpm 925")
        self.wait_controller_rpm(925, timeout=20.0)
        self.wait_remote_status_field("controller.setRPM", 925, timeout=20.0)

        self.controller_command_ok("api setweight 20500", ack_cmd="setweight")
        self.wait_remote_status_field("controller.setWeightMg", 20500, timeout=20.0)

        self.remote_command_ok("api setweight 21500")
        self.wait_controller_weight(21500, timeout=20.0)

        self.remote_command_ok("api set 1 5 130")
        self.wait_controller_menu(1, 5, 130, timeout=20.0)
        self.wait_remote_menu(1, 5, 130, timeout=20.0)

        self.remote_command_ok("api action idle_gbw")
        self.wait_controller_state("IDLE_GBW", timeout=20.0)
        self.wait_remote_status_field("controller.state", "IDLE_GBW", timeout=20.0)

        self.remote_command_ok("api action idle")
        self.wait_controller_state("IDLE", timeout=20.0)
        self.wait_remote_status_field("controller.state", "IDLE", timeout=20.0)

        for index in range(8):
            rpm = 900 + (index * 15)
            self.controller_command_ok(f"api setrpm {rpm}", ack_cmd="setrpm")
            self.wait_remote_status_field("controller.setRPM", rpm, timeout=20.0)
            status = self.remote_status()
            self.expect(status["transport"] == "WebSocket" and status["linkOnline"] and status["wsConnected"],
                        f"WebSocket soak lost link on iteration {index}: {status}")
        self.load_probe("WebSocket")

    def headless_guard_suite(self):
        status = self.controller_status()
        if not status.get("remoteRecoveryRequired"):
            print("== Headless guard suite skipped: full-size local UI available ==")
            return

        print("== Headless guard suite ==")
        payload = self.controller_command_expect_fail("api set 1 4 2", ack_cmd="set")
        self.expect("headless" in payload.get("message", "").lower(),
                    f"headless OFF rejection message was unexpected: {payload}")
        status = self.controller_status()
        self.expect(status["radioDesiredMode"] != "OFF" and status["remoteRecoveryRequired"] is True,
                    f"headless controller should reject OFF and require remote recovery: {status}")
        self.load_probe("Headless guard")

    def load_probe(self, label, samples=12):
        print(f"== Load probe: {label} ==")
        remote_before = len(self.remote.timings)
        controller_before = len(self.controller.timings)
        relock_recoveries = 0
        for _ in range(samples):
            remote_status = self.remote_status()
            controller_status = self.controller_status()
            if not remote_status["linkOnline"]:
                relock_recoveries += 1
                remote_status = self.wait_remote_transport("ESP-NOW", require_online=True, timeout=10.0)
            self.expect(controller_status["state"] in ("IDLE", "IDLE_GBW", "GRINDING", "GRINDING_GBW", "PURGING"),
                        f"unexpected controller state during {label} load probe: {controller_status}")
            time.sleep(0.15)

        remote_samples = self.remote.timings[remote_before:]
        controller_samples = self.controller.timings[controller_before:]
        for side, samples_ in (("remote", remote_samples), ("controller", controller_samples)):
            if not samples_:
                continue
            max_ms = max(duration for _, duration in samples_)
            avg_ms = sum(duration for _, duration in samples_) / len(samples_)
            print(f"   {side}: n={len(samples_)} avg={avg_ms:.1f}ms max={max_ms:.1f}ms")
            self.expect(max_ms < 1500.0, f"{side} serial/API latency too high during {label}: max={max_ms:.1f}ms")
        if relock_recoveries:
            print(f"   relock recoveries={relock_recoveries}")

    def run(self):
        if self.factory_network_reset:
            self.reset_network_for_factory_test()
        self.capture_original_state()
        try:
            if self.factory_network_reset:
                self.fresh_discovery_suite()
                self.web_mode_matrix_suite()
                self.full_size_controller_only_suite()
                self.headless_espnow_fallback_suite()
            self.headless_guard_suite()
            self.espnow_suite()
            if self.skip_websocket:
                print("== WebSocket suite skipped: factory/no-info mode has no saved Wi-Fi ==")
            else:
                self.websocket_suite()
        finally:
            self.restore_original_state()
            for console in (self.remote, self.controller):
                summary = console.timing_summary()
                print(
                    f"TIMING_SUMMARY {console.label} "
                    f"count={summary['count']} avg_ms={summary['avg_ms']:.1f} "
                    f"p95_ms={summary['p95_ms']:.1f} max_ms={summary['max_ms']:.1f}"
                )


def candidate_serial_ports():
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.SLAB_USBto*",
    ]
    ports = []
    seen = set()
    for pattern in patterns:
        for port in glob.glob(pattern):
            if port not in seen:
                seen.add(port)
                ports.append(port)
    return sorted(ports)


def read_json_response(port, command, expected_type=None, timeout=2.5):
    try:
        with serial.Serial(port, 115200, timeout=0.2) as ser:
            time.sleep(0.2)
            ser.reset_input_buffer()
            ser.write((command + "\n").encode("utf-8"))
            ser.flush()
            deadline = time.time() + timeout
            while time.time() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("{"):
                    continue
                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if expected_type is None or payload.get("type") == expected_type:
                    return payload
    except serial.SerialException:
        return None
    return None


def role_from_payload(payload):
    if not isinstance(payload, dict):
        return None
    if payload.get("type") == "device":
        role = payload.get("role")
        if role in ("remote", "controller"):
            return role
    if payload.get("type") == "status":
        data = payload.get("data")
        if isinstance(data, dict) and "transport" in data and "board" in data:
            return "remote"
        if "state" in payload and "motor" in payload and "drive" in payload:
            return "controller"
    return None


def probe_serial_port(port):
    payload = read_json_response(port, "api device", expected_type="device")
    if payload is None:
        payload = read_json_response(port, "api status", expected_type="status")
    role = role_from_payload(payload)
    return {"port": port, "role": role, "payload": payload}


def discover_boards():
    discovered = {"remote": None, "controller": None}
    for port in candidate_serial_ports():
        info = probe_serial_port(port)
        role = info.get("role")
        if role in discovered and discovered[role] is None:
            discovered[role] = info
    return discovered


def controller_mac_from_detection(discovered):
    controller = discovered.get("controller")
    if controller and isinstance(controller.get("payload"), dict):
        mac = controller["payload"].get("mac")
        if mac:
            return mac

    remote = discovered.get("remote")
    payload = remote.get("payload") if remote else None
    data = payload.get("data") if isinstance(payload, dict) else None
    peers = data.get("peers") if isinstance(data, dict) else None
    if isinstance(peers, list):
        for peer in peers:
            if isinstance(peer, dict) and peer.get("mac"):
                return peer["mac"]
    return FALLBACK_CONTROLLER_MAC


def main():
    parser = argparse.ArgumentParser(description="Run SG98 controller/remote serial regression")
    parser.add_argument("--remote-port", default=DEFAULT_REMOTE_PORT)
    parser.add_argument("--controller-port", default=DEFAULT_CONTROLLER_PORT)
    parser.add_argument("--controller-mac", default=DEFAULT_CONTROLLER_MAC)
    parser.add_argument("--skip-websocket", action="store_true")
    parser.add_argument("--factory-network-reset", action="store_true")
    args = parser.parse_args()

    remote_port = args.remote_port
    controller_port = args.controller_port
    controller_mac = args.controller_mac
    if "auto" in (remote_port, controller_port, controller_mac):
        discovered = discover_boards()
        if remote_port == "auto":
            if discovered["remote"] is None:
                print("Could not auto-detect remote USB serial port.", file=sys.stderr)
                return 1
            remote_port = discovered["remote"]["port"]
        if controller_port == "auto":
            if discovered["controller"] is None:
                print("Could not auto-detect controller USB serial port.", file=sys.stderr)
                return 1
            controller_port = discovered["controller"]["port"]
        if controller_mac == "auto":
            controller_mac = controller_mac_from_detection(discovered)
        print(f"Using remote={remote_port} controller={controller_port} controller_mac={controller_mac}")

    rig = DualBoardRegression(remote_port,
                              controller_port,
                              controller_mac,
                              skip_websocket=args.skip_websocket,
                              factory_network_reset=args.factory_network_reset)
    try:
        rig.run()
    except TestFailure as exc:
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        rig.close()

    print("ALL_DUAL_BOARD_REGRESSION_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
