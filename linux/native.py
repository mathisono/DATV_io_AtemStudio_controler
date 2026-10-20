"""ctypes declarations; every native call is serialized by the host lock."""
import ctypes as C
from pathlib import Path

U = C.c_uint32
SEND = C.CFUNCTYPE(C.c_bool, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t)
LOG = C.CFUNCTYPE(None, C.c_char_p)

class Config(C.Structure):
    _fields_ = [(k, U) for k in ("acquire", "release", "interval", "policy", "conflict", "fallback", "respectExternal")] + [
        (k, U * 6) for k in ("enabled", "activeHigh", "source", "priority")]

class Status(C.Structure):
    _fields_ = [(k, U) for k in (
        "automatic fresh connected programValid previewValid ftbValid program preview ftb transitioning "
        "version generation lastRx pending sent errors retries desired eligible remaining restart").split()] + [
        (k, U * 6) for k in ("raw", "locked", "qualifying", "inputRemaining")]

class Native:
    def __init__(self, send, log):
        self.lib = C.CDLL(str(Path(__file__).resolve().parents[1] / "build/libdatv.so"))
        signatures = {
            "create": ([SEND, LOG], C.c_void_p), "destroy": ([C.c_void_p], None),
            "configure": ([C.c_void_p, C.POINTER(Config), U], None),
            "start": ([C.c_void_p, U, U], None), "disconnect": ([C.c_void_p], None),
            "receive": ([C.c_void_p, C.c_char_p, C.c_size_t, U], None),
            "inputs": ([C.c_void_p, U, U, U], None), "tick": ([C.c_void_p, U], None),
            "restart_due": ([C.c_void_p, U], U), "mode": ([C.c_void_p, U, U], U),
            "action": ([C.c_void_p, U, U, U], U),
            "status": ([C.c_void_p, C.POINTER(Status)], None),
            "text": ([C.c_void_p, U], C.c_char_p),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.lib, "datv_" + name)
            fn.argtypes, fn.restype = args, result
        # Keep callbacks alive for the lifetime of the native instance.
        self.send_callback = SEND(lambda _, data, size: send(C.string_at(data, size)))
        self.log_callback = LOG(lambda msg: log(msg.decode("utf-8", errors="replace")))
        self.ptr = self.lib.datv_create(self.send_callback, self.log_callback)
        if not self.ptr:
            raise MemoryError("Unable to create controller core")

    def call(self, name, *args):
        return getattr(self.lib, "datv_" + name)(self.ptr, *args)

    def configure(self, value, now):
        config = Config(value["acquireMs"], value["releaseMs"], value["switchIntervalSec"] * 1000,
                        ["round_robin", "priority", "conflict"].index(value["multiPolicy"]),
                        value["conflictSource"], value["fallbackSource"], value["respectExternalControl"])
        for key in ("enabled", "activeHigh", "source", "priority"):
            setattr(config, key, (U * 6)(*(v[key] for v in value["inputs"])))
        self.call("configure", C.byref(config), now)

    def status(self):
        status = Status()
        self.call("status", C.byref(status))
        return status

    def text(self, field):
        return self.call("text", field).decode("utf-8", errors="replace")

    def close(self):
        if self.ptr:
            self.call("destroy")
            self.ptr = None
