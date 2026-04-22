#!/usr/bin/env python3
import argparse
import json
import sys
import time
from collections import deque

import serial


DEFAULT_REMOTE_PORT = "/dev/cu.usbmodem21101"
DEFAULT_CONTROLLER_PORT = "/dev/cu.usbmodem21201"
DEFAULT_CONTROLLER_MAC = "FC:01:2C:D6:38:BC"


class TestFailure(RuntimeError):
    pass


class SerialConsole:
    def __init__(self, label, port, baudrate=115200, timeout=0.2):
        self.label = label
        self.port = port
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.log = deque(maxlen=200)

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
                return payload
        raise TestFailure(f"{self.label}: timeout waiting for {type_name} after `{command}`")

    def request_controller_snapshot(self, timeout=10.0):
        self.drain(0.1)
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
        raise TestFailure(f"{self.label}: timeout waiting for controller snapshot")


class DualBoardRegression:
    def __init__(self, remote_port, controller_port, controller_mac):
        self.remote = SerialConsole("remote", remote_port)
        self.controller = SerialConsole("controller", controller_port)
        self.controller_mac = controller_mac
        self.original = {}

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

    def run(self):
        self.capture_original_state()
        try:
            self.espnow_suite()
            self.websocket_suite()
        finally:
            self.restore_original_state()


def main():
    parser = argparse.ArgumentParser(description="Run SG98 controller/remote serial regression")
    parser.add_argument("--remote-port", default=DEFAULT_REMOTE_PORT)
    parser.add_argument("--controller-port", default=DEFAULT_CONTROLLER_PORT)
    parser.add_argument("--controller-mac", default=DEFAULT_CONTROLLER_MAC)
    args = parser.parse_args()

    rig = DualBoardRegression(args.remote_port, args.controller_port, args.controller_mac)
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
