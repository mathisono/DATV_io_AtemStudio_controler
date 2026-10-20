"""Host integration tests: real shared core, PTY serial, loopback UDP and HTTP."""
import base64
import copy
import http.client
import json
import os
from pathlib import Path
import pty
import socket
import struct
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "linux"))
from controller import Controller, Server, ControlError
from native import Native
from serial_inputs import SerialInputs
import settings

def packet(flags, session, sequence=0, payload=b"", ack=0):
    return struct.pack("!6H", (flags << 11) | (12 + len(payload)), session, ack, 0, 0, sequence) + payload

def block(name, payload=b""):
    return struct.pack("!HH4s", 8 + len(payload), 0, name.encode()) + payload

def handshake(native, now=0):
    native.call("start", now, 0x1234)
    hello = packet(2, 0x1234, payload=b"\x02" + bytes(7))
    native.call("receive", hello, len(hello), now+1)
    initial = packet(1, 0x8001, 1, block("PrgI", bytes([0, 0, 0, 1])) +
                     block("PrvI", bytes([0, 0, 0, 2])) + block("InCm"))
    native.call("receive", initial, len(initial), now+2)
    native.call("tick", now+2)

class NativeIntegration(unittest.TestCase):
    def setUp(self):
        self.packets, self.events = [], []
        self.core = Native(lambda data: self.packets.append(data) or True, self.events.append)
        self.config = settings.defaults()
        self.config["acquireMs"] = self.config["releaseMs"] = 0
        self.core.configure(self.config, 0)

    def tearDown(self):
        self.core.close()

    def test_startup_requires_both_links_and_manual_confirmation(self):
        self.assertFalse(self.core.call("mode", 1, 0))
        handshake(self.core)
        self.assertFalse(self.core.call("mode", 1, 3))
        self.core.call("inputs", 61, 1, 4) # receiver 2 active low
        self.assertFalse(self.core.status().automatic)
        self.assertTrue(self.core.call("mode", 1, 5))
        self.core.call("tick", 6)
        self.assertEqual(self.core.status().desired, 2)
        self.assertEqual(self.packets[-1][16:20], b"CPgI")
        self.assertEqual(self.packets[-1][-2:], b"\x00\x02")
        self.assertEqual(self.core.status().program, 1) # no optimistic tally

    def test_stale_inputs_cancel_unacked_take_and_require_rearm(self):
        handshake(self.core)
        self.core.call("inputs", 61, 1, 4)
        self.core.call("mode", 1, 5)
        self.core.call("tick", 6)
        self.assertEqual(self.core.status().pending, 1)
        self.core.call("inputs", 0, 0, 7)
        before = len(self.packets)
        self.core.call("tick", 1000)
        self.assertEqual(len(self.packets), before, "No automatic retry or fallback after input loss")
        state = self.core.status()
        self.assertFalse(state.automatic)
        self.assertFalse(state.fresh)
        self.assertEqual(state.pending, 0)
        self.assertTrue(state.restart)
        handshake(self.core, 1010)
        self.core.call("inputs", 61, 1, 1020)
        self.assertFalse(self.core.status().automatic, "Recovery does not silently rearm")

    def test_external_takeover_and_config_pause(self):
        handshake(self.core)
        self.core.call("inputs", 62, 1, 4) # receiver 1 already on Program
        self.core.call("mode", 1, 5)
        self.core.call("tick", 6)
        external = packet(1, 0x8001, 2, block("PrgI", bytes([0, 0, 0, 4])))
        self.core.call("receive", external, len(external), 7)
        self.core.call("tick", 8)
        self.assertFalse(self.core.status().automatic)
        self.assertTrue(any("External" in e for e in self.events))
        self.core.call("mode", 1, 9)
        self.core.configure(self.config, 10)
        self.assertFalse(self.core.status().automatic)
        self.assertFalse(self.core.status().fresh)

class SerialIntegration(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        self.serial = SerialInputs(os.ttyname(self.slave))
        self.serial.poll(0)
        self.token = os.read(self.master, 128).split()[1]

    def tearDown(self):
        self.serial.close()
        os.close(self.master)
        os.close(self.slave)

    def test_partial_frame_invalid_mask_and_duplicate(self):
        os.write(self.master, b"S1 " + self.token + b" 64\nS1 deadbeef 1\n")
        self.serial.poll(.01)
        self.assertFalse(self.serial.fresh(.01))
        os.write(self.master, b"S1 " + self.token + b" 6")
        self.serial.poll(.02)
        self.assertFalse(self.serial.fresh(.02))
        os.write(self.master, b"1\n")
        self.serial.poll(.03)
        self.assertTrue(self.serial.fresh(.03))
        self.assertEqual(self.serial.mask, 61)
        os.write(self.master, b"S1 " + self.token + b" 0\n")
        self.serial.poll(.04)
        self.assertEqual(self.serial.last_snapshot, .03)
        self.assertEqual(self.serial.mask, 61)

    def test_timeout_and_reopen_discards_old_snapshots(self):
        os.write(self.master, b"S1 " + self.token + b" 62\n")
        self.serial.poll(.01)
        self.serial.poll(.1)
        old_token = os.read(self.master, 128).split()[1]
        os.write(self.master, b"S1 " + old_token + b" 62\n")
        self.serial.poll(1.1) # stall; queued reply must not hide it
        self.assertFalse(self.serial.fresh(1.1))
        self.assertIsNone(self.serial.fd)
        self.serial.poll(2.2)
        new_token = os.read(self.master, 128).split()[1]
        os.write(self.master, b"S1 " + old_token + b" 62\n")
        self.serial.poll(2.21)
        self.assertFalse(self.serial.fresh(2.21))
        os.write(self.master, b"S1 " + new_token + b" 62\n")
        self.serial.poll(2.22)
        self.assertTrue(self.serial.fresh(2.22))

    def test_overlong_and_late_frames_do_not_refresh(self):
        os.write(self.master, b"x"*100 + b"S1 " + self.token + b" 1\n")
        self.serial.poll(.1)
        self.assertFalse(self.serial.fresh(.1))
        os.write(self.master, b"S1 " + self.token + b" 1\n")
        self.serial.poll(.3)
        self.assertFalse(self.serial.fresh(.3))

class HostIntegration(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "config.json"
        self.config = settings.defaults()
        self.config["adminPassword"] = "test-password-only"
        self.config["acquireMs"] = 0
        settings.save(self.path, self.config)
        self.now = 0.0
        self.controller = Controller(self.config, self.path, clock=lambda: self.now)

    def tearDown(self):
        self.controller.close()
        self.directory.cleanup()

    def test_http_auth_csrf_validation_persistence_and_unknown_status(self):
        server = Server(("127.0.0.1", 0), self.controller)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        auth = "Basic " + base64.b64encode(b"admin:test-password-only").decode()
        def request(method, path, body=None, token=None, authenticated=True):
            conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            headers = {"Content-Type": "application/json"}
            if authenticated:
                headers["Authorization"] = auth
            if token is not None:
                headers["X-CSRF-Token"] = token
            conn.request(method, path, body=None if body is None else json.dumps(body), headers=headers)
            response = conn.getresponse()
            code, result = response.status, json.loads(response.read())
            conn.close()
            return code, result
        try:
            self.assertEqual(request("GET", "/api/status", authenticated=False)[0], 401)
            code, result = request("GET", "/api/status")
            self.assertEqual(code, 200)
            self.assertNotIn("adminPassword", result["config"])
            self.assertFalse(result["inputs"][0]["known"])
            self.assertIsNone(result["inputs"][0]["locked"])
            token = result["csrfToken"]
            self.assertEqual(request("POST", "/api/mode", {"mode": "auto"})[0], 403)
            self.assertEqual(request("POST", "/api/mode", {"mode": "auto"}, token)[0], 409)
            invalid = settings.public(self.config)
            invalid["acquireMs"] = True
            self.assertEqual(request("POST", "/api/config", invalid, token)[0], 400)
            valid = settings.public(self.config)
            valid["multiPolicy"] = "conflict"
            self.assertEqual(request("POST", "/api/config", valid, token)[0], 200)
            saved = settings.validate(json.loads(self.path.read_text()))
            self.assertEqual(saved["multiPolicy"], "conflict")
            self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
            valid["adminPassword"] = "replacement-test-password"
            self.assertTrue(request("POST", "/api/config", valid, token)[1]["reauthRequired"])
            self.assertEqual(request("GET", "/api/status")[0], 401)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_real_serial_and_udp_flow_with_host_stall(self):
        master, slave = pty.openpty()
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        peer.bind(("127.0.0.1", 9910))
        peer.settimeout(1)
        c = self.controller
        c.serial.path = os.ttyname(slave)
        c.config["atemIp"] = "127.0.0.1" # test peer, never accepted by user configuration
        try:
            c.tick()
            query = os.read(master, 128).split()[1]
            hello, address = peer.recvfrom(2048)
            nonce = struct.unpack_from("!H", hello, 2)[0]
            peer.sendto(packet(2, nonce, payload=b"\x02"+bytes(7)), address)
            peer.sendto(packet(1, 0x8001, 1, block("PrgI", bytes([0,0,0,1])) +
                               block("PrvI", bytes([0,0,0,2])) + block("InCm")), address)
            os.write(master, b"S1 " + query + b" 61\n")
            self.now = .02
            c.tick()
            self.assertTrue(c.status()["atem"]["connected"])
            self.assertTrue(c.status()["arduino"]["fresh"])
            c.mutate("/api/mode", {"mode": "auto"})
            self.now = .03
            c.tick()
            self.assertEqual(c.native.status().pending, 1)
            commands = []
            peer.setblocking(False)
            while True:
                try:
                    data, _ = peer.recvfrom(2048)
                    if data[16:20] == b"CPgI":
                        commands.append(data)
                except BlockingIOError:
                    break
            self.assertEqual(len(commands), 1)
            self.assertEqual(commands[0][-2:], b"\x00\x02")
            self.now = 1.2
            c.tick()
            self.assertEqual(c.status()["mode"], "manual")
            self.assertFalse(c.status()["arduino"]["fresh"])
            self.assertEqual(c.native.status().pending, 0)
            while True:
                try:
                    data, _ = peer.recvfrom(2048)
                    self.assertNotEqual(data[16:20], b"CPgI", "No stale Program retries after host stall")
                except BlockingIOError:
                    break
        finally:
            peer.close()
            os.close(master)
            os.close(slave)

    def test_validation_retains_serial_identity_and_rejects_bad_config(self):
        valid = settings.public(self.config)
        valid["serialDevice"] = "/dev/other-device"
        self.assertEqual(settings.validate(valid, self.config)["serialDevice"], self.config["serialDevice"])
        for key, value in [("switchIntervalSec", 0), ("atemIp", "127.0.0.1"), ("acquireMs", 1.5), ("wifiSsid", "network")]:
            invalid = copy.deepcopy(valid)
            invalid[key] = value
            with self.assertRaises(ValueError):
                settings.validate(invalid, self.config)
        valid["inputs"][1]["source"] = 1
        with self.assertRaises(ValueError):
            settings.validate(valid, self.config)

if __name__ == "__main__":
    unittest.main()
