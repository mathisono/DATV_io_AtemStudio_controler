"""Validated local configuration. Network setup belongs to Linux Mint."""
import copy
import ipaddress
import json
import os
from pathlib import Path
import secrets
import tempfile

def defaults():
    return {
        "version": 1, "atemIp": "", "hostname": "datv-lattepanda",
        "wifiSsid": "", "wifiPasswordSet": False,
        "adminPassword": secrets.token_urlsafe(18),
        "serialDevice": "/dev/serial/by-id/REPLACE_WITH_ONBOARD_ARDUINO",
        "acquireMs": 1000, "releaseMs": 2000, "switchIntervalSec": 300,
        "multiPolicy": "round_robin", "conflictSource": 0, "fallbackSource": 0,
        "respectExternalControl": True,
        "inputs": [{"label": f"Receiver {i+1}", "enabled": True, "activeHigh": False,
                    "source": i+1, "priority": i+1} for i in range(6)]}

def integer(value, lo, hi, name):
    if type(value) is not int or not lo <= value <= hi:
        raise ValueError(f"{name} must be an integer from {lo} to {hi}")
    return value

def text(value, lo, hi, name):
    if not isinstance(value, str) or not lo <= len(value.encode("utf-8")) <= hi or any(ord(c) < 32 for c in value):
        raise ValueError(f"Invalid {name}")
    return value

def validate(value, previous=None):
    if not isinstance(value, dict):
        raise ValueError("Expected a JSON object")
    result = copy.deepcopy(previous if previous is not None else defaults())
    address = text(value.get("atemIp"), 0, 15, "ATEM IPv4 address")
    if address:
        ip = ipaddress.IPv4Address(address)
        if ip.is_loopback or ip.is_multicast or int(ip) >> 24 in (0, 255) or ip.is_reserved:
            raise ValueError("ATEM address must be a unicast IPv4 address")
    result["atemIp"] = address
    for key, lo, hi in [("acquireMs", 0, 60000), ("releaseMs", 0, 60000),
                        ("switchIntervalSec", 1, 86400), ("conflictSource", 0, 6), ("fallbackSource", 0, 6)]:
        result[key] = integer(value.get(key), lo, hi, key)
    if value.get("multiPolicy") not in ("round_robin", "priority", "conflict"):
        raise ValueError("Unknown multiple-lock policy")
    result["multiPolicy"] = value["multiPolicy"]
    if type(value.get("respectExternalControl")) is not bool:
        raise ValueError("respectExternalControl must be boolean")
    result["respectExternalControl"] = value["respectExternalControl"]
    inputs = value.get("inputs")
    if not isinstance(inputs, list) or len(inputs) != 6:
        raise ValueError("Exactly six input configurations are required")
    result["inputs"] = []
    used = set()
    for item in inputs:
        if not isinstance(item, dict):
            raise ValueError("Invalid input configuration")
        entry = {"label": text(item.get("label"), 1, 32, "receiver label")}
        for key in ("source", "priority"):
            entry[key] = integer(item.get(key), 1, 6, key)
        for key in ("enabled", "activeHigh"):
            if type(item.get(key)) is not bool:
                raise ValueError(f"{key} must be boolean")
            entry[key] = item[key]
        if entry["enabled"]:
            if entry["source"] in used:
                raise ValueError("Enabled receivers must map to different ATEM inputs")
            used.add(entry["source"])
        result["inputs"].append(entry)
    password = value.get("adminPassword", "")
    if password or previous is None:
        result["adminPassword"] = text(password, 12, 63, "administrator password")
    elif not isinstance(password, str):
        raise ValueError("Invalid administrator password")
    # Only local file loading can change the serial path. Never accept arbitrary
    # OS device paths, network settings, or host commands from the dashboard.
    if previous is None:
        if value.get("version") != 1:
            raise ValueError("Unsupported configuration version")
        result["serialDevice"] = text(value.get("serialDevice"), 6, 512, "serialDevice")
        if not result["serialDevice"].startswith("/dev/"):
            raise ValueError("serialDevice must be an explicit /dev/ device path")
    elif any(value.get(k) for k in ("wifiSsid", "wifiPassword", "clearWifiPassword")):
        raise ValueError("Manage networking in Linux Mint, not this dashboard")
    return result

def public(config):
    return {key: copy.deepcopy(value) for key, value in config.items()
            if key not in ("adminPassword", "serialDevice", "version")}

def save(path, config):
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix=".datv-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            os.fchmod(stream.fileno(), 0o600)
            json.dump(config, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
