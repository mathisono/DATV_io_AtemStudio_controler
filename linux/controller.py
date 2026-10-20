#!/usr/bin/env python3
"""LattePanda host: USB CDC inputs, shared ATEM core, local operator dashboard."""
import argparse
import base64
from collections import deque
import fcntl
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
import os
from pathlib import Path
import secrets
import signal
import socket
import threading
import time

from native import Native
from serial_inputs import SerialInputs, PINS
import settings

ROOT = Path(__file__).resolve().parents[1]

class ControlError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message

class Controller:
    def __init__(self, config, config_path, clock=time.monotonic):
        self.clock = clock
        self.started = clock()
        self.lock = threading.RLock()
        self.config, self.config_path = config, config_path
        self.events = deque(maxlen=64)
        self.csrf = secrets.token_hex(24)
        self.sock = None
        self.udp_retry = 0.0
        self.serial = SerialInputs(config["serialDevice"])
        self.native = Native(self.send, self.log)
        self.native.configure(config, self.now_ms())
        self.last_program = None
        self.last_locks = None
        self.log("Controller ready; automatic routing starts paused")

    def now_ms(self):
        return int((self.clock() - self.started) * 1000) & 0xffffffff

    def log(self, message):
        self.events.appendleft({"uptimeMs": int((self.clock() - self.started) * 1000), "message": message})
        logging.info("%s", message)

    def send(self, packet):
        try:
            return self.sock is not None and self.sock.send(packet) == len(packet)
        except OSError:
            return False

    def close_udp(self):
        if self.sock is not None:
            self.sock.close()
        self.sock = None

    def connect_udp(self, now):
        self.close_udp()
        self.native.call("disconnect")
        self.udp_retry = now + 1.5
        if not self.config["atemIp"]:
            return
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setblocking(False)
            self.sock.bind(("0.0.0.0", 0))
            # A connected UDP socket filters foreign addresses and source ports.
            self.sock.connect((self.config["atemIp"], 9910))
            self.native.call("start", self.now_ms(), secrets.randbelow(32767) + 1)
        except OSError as exc:
            self.close_udp()
            self.log(f"ATEM socket unavailable; retrying: {exc}")

    def check_stale(self):
        if not self.serial.fresh(self.clock()):
            self.native.call("inputs", 0, 0, self.now_ms())

    def tick(self):
        with self.lock:
            now = self.clock()
            # Invalidate before reading buffers: a scheduler stall cannot revive
            # old locks or let an old automatic command retransmit first.
            self.check_stale()
            self.serial.poll(now)
            self.native.call("inputs", self.serial.mask, self.serial.fresh(now), self.now_ms())
            if ((self.sock is None and now >= self.udp_retry) or
                    (self.sock is not None and self.native.call("restart_due", self.now_ms()))):
                self.connect_udp(now)
            if self.sock is not None:
                for _ in range(16):
                    try:
                        packet = self.sock.recv(2048)
                    except BlockingIOError:
                        break
                    except OSError:
                        self.close_udp()
                        self.native.call("disconnect")
                        self.udp_retry = now + 1.5
                        break
                    self.native.call("receive", packet, len(packet), self.now_ms())
            self.native.call("tick", self.now_ms())
            state = self.native.status()
            program = state.program if state.connected and state.programValid else None
            if program != self.last_program:
                self.log(f"ATEM confirms Program {program}" if program is not None else "ATEM feedback unavailable")
                self.last_program = program
            locks = tuple(state.locked) if state.fresh else None
            if locks != self.last_locks:
                if locks is not None:
                    for i, locked in enumerate(locks):
                        if self.last_locks is None or self.last_locks[i] != locked:
                            self.log(f"{self.config['inputs'][i]['label']}: {'lock qualified' if locked else 'no qualified lock'}")
                self.last_locks = locks

    def status(self):
        self.check_stale()
        s = self.native.status()
        fresh = bool(s.fresh)
        ip = self.sock.getsockname()[0] if self.sock is not None else ""
        return {
            "csrfToken": self.csrf, "uptimeMs": int((self.clock()-self.started)*1000),
            "mode": "auto" if s.automatic else "manual", "board": "LattePanda 3 Delta / Linux",
            "capabilities": {"osNetworking": True, "restart": False},
            "network": {"link": self.sock is not None, "ip": ip, "interface": "Linux networking"},
            "arduino": {"fresh": fresh, "device": self.serial.path,
                        "error": self.serial.error,
                        "ageMs": None if self.serial.last_snapshot is None else int((self.clock()-self.serial.last_snapshot)*1000)},
            "atem": {"configured": bool(self.config["atemIp"]), "connected": bool(s.connected),
                     "status": self.native.text(0), "error": self.native.text(1),
                     "program": s.program if s.connected and s.programValid else None,
                     "preview": s.preview if s.connected and s.previewValid else None,
                     "fadeToBlack": bool(s.ftb) if s.connected and s.ftbValid else None,
                     "inTransition": bool(s.connected and s.transitioning),
                     "product": self.native.text(2), "version": s.version, "lastRxMs": s.lastRx,
                     "commandsSent": s.sent, "errors": s.errors, "pendingCommands": s.pending, "retransmissions": s.retries},
            "automatic": {"desiredSource": None if s.desired == 65535 else s.desired,
                          "reason": self.native.text(3) if fresh else "Arduino status unknown; automatic routing is paused.",
                          "eligibleCount": s.eligible if fresh else 0, "remainingMs": s.remaining},
            "inputs": [{**rule, "index": i, "gpio": f"D{PINS[i]}", "known": fresh,
                        "rawHigh": bool(s.raw[i]) if fresh else None,
                        "locked": bool(s.locked[i]) if fresh else None,
                        "qualifying": bool(s.qualifying[i]) if fresh else False,
                        "remainingMs": s.inputRemaining[i] if fresh else 0}
                       for i, rule in enumerate(self.config["inputs"])],
            "events": list(self.events), "config": settings.public(self.config)}

    def mutate(self, path, body):
        now = self.now_ms()
        self.check_stale()
        if path == "/api/mode":
            if body.get("mode") not in ("manual", "auto"):
                raise ValueError("Mode must be auto or manual")
            if not self.native.call("mode", body["mode"] == "auto", now):
                raise ControlError(409, "Fresh Arduino inputs and synchronized ATEM feedback with no pending commands are required")
            self.log(f"Operator selected {body['mode']} mode")
            return {"ok": True, "message": "Automatic routing " + ("resumed" if body["mode"] == "auto" else "paused")}
        if path == "/api/action":
            actions = ["program", "preview", "cut", "auto", "ftb"]
            action = body.get("action")
            if action not in actions:
                raise ValueError("Unknown ATEM action")
            source = settings.integer(body.get("source"), 0, 6, "source") if action in actions[:2] else 0
            if not self.native.call("action", actions.index(action), source, now):
                raise ControlError(409, "Command not queued. Wait for synchronized ATEM feedback and retry")
            self.log(f"Manual ATEM action: {action} {source}")
            return {"ok": True, "message": "Command accepted; watch ATEM feedback for confirmation"}
        if path == "/api/config":
            updated = settings.validate(body, self.config)
            settings.save(self.config_path, updated)
            address_changed = updated["atemIp"] != self.config["atemIp"]
            password_changed = updated["adminPassword"] != self.config["adminPassword"]
            self.config = updated
            self.native.configure(updated, now)
            if address_changed:
                self.connect_udp(self.clock())
            if password_changed:
                self.csrf = secrets.token_hex(24)
            self.log("Settings saved; verify input qualification before resuming Auto")
            return {"ok": True, "rebootRequired": False, "reauthRequired": password_changed,
                    "message": "Settings saved and applied. Automatic routing is paused."}
        raise ControlError(404, "Not found")

    def close(self):
        with self.lock:
            self.native.call("mode", 0, self.now_ms())
            self.close_udp()
            self.serial.close()
            self.native.close()

class Server(ThreadingHTTPServer):
    # Join bounded, timeout-limited handlers before destroying the native core.
    daemon_threads = False
    allow_reuse_address = True

    def __init__(self, address, controller):
        self.controller = controller
        self.slots = threading.BoundedSemaphore(16)
        super().__init__(address, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()

class Handler(BaseHTTPRequestHandler):
    server_version = "DATV"

    def setup(self):
        super().setup()
        self.connection.settimeout(2)

    def log_message(self, *_):
        pass

    def reply(self, code, body, mime="application/json"):
        if isinstance(body, dict):
            body = json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy", "default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        if code == 401:
            self.send_header("WWW-Authenticate", 'Basic realm="DATV ATEM controller", charset="UTF-8"')
        self.end_headers()
        self.wfile.write(body)

    def authenticate(self):
        header = self.headers.get("Authorization", "")
        supplied = b""
        if header.startswith("Basic "):
            try:
                supplied = base64.b64decode(header[6:], validate=True)
            except ValueError:
                pass
        expected = ("admin:" + self.server.controller.config["adminPassword"]).encode("utf-8")
        return hmac.compare_digest(supplied, expected)

    def do_GET(self):
        c = self.server.controller
        with c.lock:
            authenticated = self.authenticate()
            if authenticated and self.path == "/api/status":
                response = c.status()
            else:
                response = None
        if not authenticated:
            self.reply(401, {"ok": False, "error": "Sign-in required"})
            return
        if response is not None:
            self.reply(200, response)
        elif self.path in ("/", "/app.js", "/app.css"):
            asset = {"/": ("index.html", "text/html; charset=utf-8"),
                     "/app.js": ("app.js", "application/javascript"),
                     "/app.css": ("app.css", "text/css; charset=utf-8")}[self.path]
            self.reply(200, (ROOT / "web" / asset[0]).read_bytes(), asset[1])
        else:
            self.reply(404, {"ok": False, "error": "Not found"})

    def do_POST(self):
        c = self.server.controller
        try:
            # Read sockets outside the control lock so a slow browser cannot
            # delay input monitoring or the ATEM session.
            with c.lock:
                authenticated = self.authenticate()
            if not authenticated:
                raise ControlError(401, "Sign-in required")
            if self.headers.get("Transfer-Encoding") or len(self.headers.get_all("Content-Length", [])) != 1:
                raise ControlError(400, "Exactly one Content-Length is required")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 8192:
                raise ControlError(413, "JSON body missing or too large")
            if self.headers.get_content_type() != "application/json":
                raise ControlError(415, "Use application/json")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("Expected a JSON object")
            with c.lock:
                if not self.authenticate():
                    raise ControlError(401, "Sign-in required")
                if not hmac.compare_digest(self.headers.get("X-CSRF-Token", "").encode("utf-8"), c.csrf.encode("ascii")):
                    raise ControlError(403, "Missing or stale control token; reload the page")
                response = c.mutate(self.path, body)
            self.reply(200, response)
        except ControlError as exc:
            self.reply(exc.status, {"ok": False, "error": exc.message})
        except (ValueError, UnicodeError) as exc:
            self.reply(400, {"ok": False, "error": str(exc)})
        except OSError:
            self.reply(500, {"ok": False, "error": "Request or settings write failed"})

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("config.local.json"))
    parser.add_argument("--bind", default=os.environ.get("DATV_BIND", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("DATV_PORT", "8080")))
    parser.add_argument("--init", action="store_true", help="Create a new configuration; never overwrite one")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    if args.init:
        config = settings.defaults()
        # Exclusive create, including in the presence of another initializer.
        fd = os.open(args.config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as stream:
            json.dump(config, stream, indent=2)
            stream.write("\n")
        print(f"Created {args.config}. Set serialDevice before starting.\nUsername: admin\nPassword: {config['adminPassword']}")
        return
    config = settings.validate(json.loads(args.config.read_text()))
    # Prevent two processes using the same configuration from competing on air.
    with open(str(args.config) + ".lock", "a") as instance_lock:
        fcntl.flock(instance_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        controller = Controller(config, args.config)
        server = None
        stopped = threading.Event()
        try:
            server = Server((args.bind, args.port), controller)
            for sig in (signal.SIGINT, signal.SIGTERM):
                signal.signal(sig, lambda *_: stopped.set())
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            logging.info("Dashboard at http://%s:%s/ (automatic routing paused)", args.bind, args.port)
            while not stopped.is_set():
                controller.tick()
                stopped.wait(0.01)
        finally:
            if server is not None:
                server.shutdown()
                server.server_close()
            controller.close()

if __name__ == "__main__":
    main()
