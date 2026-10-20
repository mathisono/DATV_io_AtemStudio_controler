"""Linux USB CDC input reader with bounded frames and challenge-based freshness."""
import array
import errno
import fcntl
import os
import re
import secrets
import termios
import tty

PINS = (4, 5, 6, 7, 8, 9)
TIMEOUT = 1.0
REPLY_TIMEOUT = 0.25

class SerialInputs:
    def __init__(self, path):
        self.path = path
        self.fd = None
        self.buffer = bytearray()
        self.discarding = False
        self.pending = None
        self.next_poll = 0.0
        self.retry_at = 0.0
        self.opened_at = 0.0
        self.last_snapshot = None
        self.mask = 0
        self.error = "Waiting for Arduino"

    def fresh(self, now):
        return self.fd is not None and self.last_snapshot is not None and now - self.last_snapshot < TIMEOUT

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
        self.fd = None
        self.last_snapshot = None
        self.pending = None
        self.buffer.clear()
        self.discarding = False

    def _open(self, now):
        fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            tty.setraw(fd, termios.TCSANOW)
            attrs = termios.tcgetattr(fd)
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[4] = attrs[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIOFLUSH)
            try:
                fcntl.ioctl(fd, termios.TIOCMBIS, array.array("i", [termios.TIOCM_DTR]))
            except OSError as exc:
                if exc.errno != errno.ENOTTY: # pseudo-terminal used in tests
                    raise
        except BaseException:
            os.close(fd)
            raise
        self.fd, self.opened_at, self.next_poll = fd, now, now
        self.error = "Waiting for a current Arduino snapshot"

    def _line(self, line, now):
        match = re.fullmatch(rb"S1 ([0-9a-f]{8}) ([0-9]{1,2})\r?", line)
        if not match or self.pending is None:
            return
        token, sent = self.pending
        if match[1] != token or now - sent > REPLY_TIMEOUT:
            return
        mask = int(match[2])
        if mask > 63:
            return
        self.mask, self.last_snapshot, self.pending = mask, now, None
        self.error = ""

    def poll(self, now):
        try:
            if self.fd is None:
                if now < self.retry_at:
                    return
                self._open(now)
            # Never let buffered replies conceal a lost heartbeat or host stall.
            baseline = self.last_snapshot if self.last_snapshot is not None else self.opened_at
            if now - baseline >= TIMEOUT:
                raise OSError("Arduino heartbeat timed out")
            for _ in range(8):
                try:
                    data = os.read(self.fd, 512)
                except BlockingIOError:
                    break
                if not data:
                    raise OSError("Arduino USB connection closed")
                for ch in data:
                    if ch == 10:
                        if not self.discarding:
                            self._line(bytes(self.buffer), now)
                        self.buffer.clear()
                        self.discarding = False
                    elif not self.discarding:
                        if len(self.buffer) >= 64:
                            self.buffer.clear()
                            self.discarding = True
                        else:
                            self.buffer.append(ch)
            if self.pending and now - self.pending[1] >= REPLY_TIMEOUT:
                self.pending = None
            if self.pending is None and now >= self.next_poll:
                token = secrets.token_hex(4).encode("ascii")
                request = b"Q1 " + token + b"\n"
                if os.write(self.fd, request) != len(request):
                    raise OSError("Incomplete Arduino query write")
                self.pending, self.next_poll = (token, now), now + 0.05
        except (OSError, termios.error) as exc:
            self.error = str(exc)
            self.close()
            self.retry_at = now + 1.0
