"""Station settings encrypted for the Windows account running the background task."""

from __future__ import annotations

import ctypes
import json
import os
import re
import ssl
import tempfile
import uuid
from dataclasses import asdict
from pathlib import Path
from urllib.parse import urlsplit

from station_core import Settings, build_set


def validate(settings: Settings) -> None:
    try:
        uuid.UUID(settings.station_id)
        origin = urlsplit(settings.approved_origin)
        if (origin.scheme != "https" or not origin.hostname or not settings.approved_origin.isascii()
                or origin.username or origin.password
                or origin.path or origin.query or origin.fragment
                or settings.approved_origin != f"https://{origin.netloc}"):
            raise ValueError()
        if not (1024 <= settings.listen_port <= 65535
                and 0 <= settings.usb_vid <= 65535 and 0 <= settings.usb_pid <= 65535
                and settings.usb_location and len(settings.usb_location) <= 128):
            raise ValueError()
        ssid = settings.wifi_ssid.encode("utf-8")
        password = settings.wifi_password.encode("utf-8")
        if not (1 <= len(ssid) <= 32 and len(password) <= 64):
            raise ValueError()
        if password and (len(password) < 8 or (len(password) == 64
                         and not re.fullmatch(rb"[0-9a-fA-F]{64}", password))):
            raise ValueError()
        if any(c < 32 or c == 127 for c in ssid + password):
            raise ValueError()
        host = settings.backend_host
        if (not 1 <= len(host) <= 253 or not re.fullmatch(r"[A-Za-z0-9.-]+", host)
                or host[0] in ".-" or host[-1] in ".-" or ".." in host
                or ".-" in host or "-." in host or not 1 <= settings.backend_port <= 65535):
            raise ValueError()
        if settings.tls:
            if not settings.ca_pem or len(settings.ca_pem.encode("utf-8")) > 3071:
                raise ValueError()
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.load_verify_locations(cadata=settings.ca_pem)
        elif settings.ca_pem:
            raise ValueError()
        if len(settings.grant_key) < 32:
            raise ValueError()
        build_set(settings, "11111111-1111-4111-8111-111111111111", 1, "A" * 64)
    except (OverflowError, TypeError, UnicodeError, ssl.SSLError, ValueError) as exc:
        raise ValueError("Invalid station configuration") from None


class DPAPIProtector:
    """Uses the current Windows user's DPAPI key; no plaintext config is written."""

    class Blob(ctypes.Structure):
        _fields_ = [("length", ctypes.c_uint32), ("data", ctypes.POINTER(ctypes.c_ubyte))]

    def __init__(self) -> None:
        if os.name != "nt":
            raise RuntimeError("The station requires Windows DPAPI")
        self.crypt = ctypes.WinDLL("crypt32", use_last_error=True)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        blob = ctypes.POINTER(self.Blob)
        self.crypt.CryptProtectData.argtypes = [blob, ctypes.c_wchar_p, ctypes.c_void_p,
                                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, blob]
        self.crypt.CryptProtectData.restype = ctypes.c_int
        self.crypt.CryptUnprotectData.argtypes = [blob, ctypes.c_void_p, ctypes.c_void_p,
                                                  ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, blob]
        self.crypt.CryptUnprotectData.restype = ctypes.c_int
        self.kernel.LocalFree.argtypes = [ctypes.c_void_p]
        self.kernel.LocalFree.restype = ctypes.c_void_p

    def _transform(self, payload: bytes, protect: bool) -> bytes:
        buffer = ctypes.create_string_buffer(payload)
        source = self.Blob(len(payload), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_ubyte)))
        target = self.Blob()
        action = self.crypt.CryptProtectData if protect else self.crypt.CryptUnprotectData
        if not action(ctypes.byref(source), None, None, None, None, 0, ctypes.byref(target)):
            raise RuntimeError("Windows DPAPI could not process the station settings")
        try:
            return ctypes.string_at(target.data, target.length)
        finally:
            self.kernel.LocalFree(ctypes.cast(target.data, ctypes.c_void_p))

    def protect(self, payload: bytes) -> bytes:
        return self._transform(payload, True)

    def unprotect(self, payload: bytes) -> bytes:
        return self._transform(payload, False)


def default_path() -> Path:
    root = os.environ.get("LOCALAPPDATA")
    if not root:
        raise RuntimeError("LOCALAPPDATA is unavailable")
    return Path(root) / "AuktionshusetUsbStation" / "station.bin"


def save(settings: Settings, path: Path, protector: object, replace: bool = False) -> None:
    validate(settings)
    if path.exists() and not replace:
        raise FileExistsError("Station settings already exist")
    data = asdict(settings)
    data["grant_key"] = settings.grant_key.hex()
    encrypted = protector.protect(json.dumps(data, separators=(",", ":")).encode("utf-8"))
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".station-", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(encrypted)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def load(path: Path, protector: object) -> Settings:
    data = json.loads(protector.unprotect(path.read_bytes()))
    data["grant_key"] = bytes.fromhex(data["grant_key"])
    settings = Settings(**data)
    validate(settings)
    return settings
