"""Fixed-port serial session and the firmware's AHCFG/1 protocol."""

from __future__ import annotations

import hashlib
import hmac
import re
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Any


UUID = re.compile(r"[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}\Z")
SECRET = re.compile(r"[0-9a-fA-F]{64}\Z")
STATUS = re.compile(r"AHCFG/1 STATUS CONFIGURED ([0-9a-f-]{36}) ([1-9][0-9]*) ([1-9][0-9]*)\Z")
ACK = re.compile(r"AHCFG/1 ACK CONFIGURED ([0-9a-f-]{36}) ([1-9][0-9]*) ([1-9][0-9]*)\Z")
ACK_UPDATED = re.compile(r"AHCFG/1 ACK UPDATED ([0-9a-f-]{36}) ([1-9][0-9]*) ([1-9][0-9]*)\Z")
ERROR = re.compile(r"AHCFG/1 ERROR ([A-Z_]+)\Z")
VERIFY_READY = re.compile(r"AHCFG/1 VERIFY READY ([0-9a-f-]{36}) ([1-9][0-9]*) ([1-9][0-9]*)\Z")
VERIFY_ERROR = re.compile(r"AHCFG/1 VERIFY ERROR (WIFI|CLOCK|BACKEND|TLS|CREDENTIAL|RATE_LIMIT|UNCONFIGURED)\Z")


@dataclass(frozen=True)
class Settings:
    station_id: str
    approved_origin: str
    listen_port: int
    usb_vid: int
    usb_pid: int
    usb_location: str
    wifi_ssid: str
    wifi_password: str
    backend_host: str
    backend_port: int
    tls: bool
    ca_pem: str
    grant_key: bytes


class StationError(Exception):
    def __init__(self, code: str, http_status: int = 409):
        super().__init__(code)
        self.code = code
        self.http_status = http_status


def valid_identity(device_id: Any, number: Any, secret: Any) -> bool:
    return (isinstance(device_id, str) and UUID.fullmatch(device_id) is not None
            and uuid.UUID(device_id).int != 0 and type(number) is int
            and 0 < number <= 2_147_483_647 and isinstance(secret, str)
            and SECRET.fullmatch(secret) is not None)


def hex_field(value: str) -> str:
    return value.encode("utf-8").hex().upper() or "-"


def build_set(settings: Settings, device_id: str, number: int, secret: str) -> bytes:
    fields = [device_id, str(number), secret, hex_field(settings.wifi_ssid),
              hex_field(settings.wifi_password), hex_field(settings.backend_host),
              str(settings.backend_port), "1" if settings.tls else "0",
              hex_field(settings.ca_pem)]
    line = "AHCFG/1 SET " + " ".join(fields) + "\n"
    if len(line.encode("ascii")) > 8192:
        raise StationError("invalid_configuration", 500)
    return line.encode("ascii")


def grant_message(settings: Settings, expires: int, connection_id: str,
                  device_id: str, number: int, secret: str) -> bytes:
    digest = hashlib.sha256(secret.encode("ascii")).hexdigest()
    return (f"AHSTATION/1\n{settings.station_id}\n{settings.approved_origin}\n{expires}\n"
            f"{connection_id}\n{device_id}\n{number}\n{digest}").encode("ascii")


def verify_grant(settings: Settings, grant: str | None, connection_id: str,
                 device_id: str, number: int, secret: str, now: int | None = None) -> bool:
    if not grant or len(grant) > 128:
        return False
    parts = grant.split(".")
    if (len(parts) != 3 or parts[0] != "v1" or not parts[1].isdigit()
            or not re.fullmatch(r"[0-9a-f]{64}", parts[2])):
        return False
    expires = int(parts[1])
    current = int(time.time()) if now is None else now
    if expires < current or expires > current + 120:
        return False
    expected = hmac.new(settings.grant_key,
                        grant_message(settings, expires, connection_id, device_id, number, secret),
                        hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected, parts[2])


def verification_grant(settings: Settings, connection_id: str,
                       device_id: str, number: int, hardware_id: str, version: int, now: int | None = None) -> str:
    expires = (int(time.time()) if now is None else now) + 90
    message = (f"AHSTATION/1/VERIFIED\n{settings.station_id}\n{settings.approved_origin}\n{expires}\n"
               f"{connection_id}\n{device_id}\n{number}\n{hardware_id}\n{version}").encode("ascii")
    digest = hmac.new(settings.grant_key, message, hashlib.sha256).hexdigest()
    return f"v1.{expires}.{digest}"


def presence_grant(settings: Settings, connection_id: str, hardware_id: str,
                   state: str, device_id: str = "-", now: int | None = None) -> str:
    expires = (int(time.time()) if now is None else now) + 90
    message = (f"AHSTATION/1/PRESENT\n{settings.station_id}\n{settings.approved_origin}\n{expires}\n"
               f"{connection_id}\n{hardware_id}\n{state}\n{device_id}").encode("ascii")
    digest = hmac.new(settings.grant_key, message, hashlib.sha256).hexdigest()
    return f"v1.{expires}.{digest}"


class PySerialBackend:
    def ports(self) -> list[Any]:
        from serial.tools import list_ports
        return list(list_ports.comports())

    def open(self, device: str) -> Any:
        import serial
        return serial.Serial(device, baudrate=115200, timeout=0.1, write_timeout=1)


class Station:
    def __init__(self, settings: Settings, serial_backend: Any | None = None):
        self.settings = settings
        self.backend = serial_backend if serial_backend is not None else PySerialBackend()
        self.lock = threading.RLock()
        self.connection: Any | None = None
        self.fingerprint: tuple[str, str] | None = None
        self.connection_id: str | None = None
        self.device: dict[str, Any] | None = None
        self.verification = "pending"
        self.last_verification = 0.0
        self.verified_identity: tuple[str, int] | None = None

    def _matching_port(self) -> Any | None:
        matches = [port for port in self.backend.ports()
                   if port.vid == self.settings.usb_vid and port.pid == self.settings.usb_pid
                   and port.location == self.settings.usb_location and port.serial_number]
        return matches[0] if len(matches) == 1 else None

    @staticmethod
    def _fingerprint(port: Any) -> tuple[str, str]:
        return port.device, port.serial_number

    def _close(self) -> None:
        if self.connection is not None:
            try:
                self.connection.close()
            except Exception:
                pass
        self.connection = self.fingerprint = self.connection_id = self.device = None
        self.verification = "pending"
        self.last_verification = 0.0
        self.verified_identity = None

    def _read_response(self, prefixes: tuple[str, ...], timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.connection.readline(8192)
            if not line:
                continue
            text = line.decode("ascii", errors="ignore").strip()
            if text.startswith(prefixes):
                return text
        return None

    def _status(self) -> dict[str, Any] | None:
        self.connection.reset_input_buffer()
        self.connection.write(b"AHCFG/1 STATUS\n")
        self.connection.flush()
        line = self._read_response(("AHCFG/1 STATUS ",), 1.5)
        if line == "AHCFG/1 STATUS UNCONFIGURED":
            return {"state": "unconfigured"}
        match = STATUS.fullmatch(line or "")
        if match and UUID.fullmatch(match[1]) and uuid.UUID(match[1]).int != 0:
            return {"state": "configured", "deviceId": match[1], "deviceNumber": int(match[2]),
                    "credentialVersion": int(match[3])}
        return None

    def _verify(self, device: dict[str, Any]) -> str:
        port = self._matching_port()
        if port is None or self._fingerprint(port) != self.fingerprint:
            raise StationError("stale_connection")
        self.connection.reset_input_buffer()
        self.connection.write(b"AHCFG/1 VERIFY\n")
        self.connection.flush()
        line = self._read_response(("AHCFG/1 VERIFY ",), 50.0)
        port = self._matching_port()
        if port is None or self._fingerprint(port) != self.fingerprint:
            raise StationError("stale_connection")
        ready = VERIFY_READY.fullmatch(line or "")
        if ready and ready[1] == device["deviceId"] and int(ready[2]) == device["deviceNumber"] and \
                int(ready[3]) == device["credentialVersion"]:
            return "ready"
        error = VERIFY_ERROR.fullmatch(line or "")
        if error:
            return error[1].lower()
        return "backend"

    def poll(self) -> None:
        with self.lock:
            try:
                port = self._matching_port()
                if port is None:
                    self._close()
                    return
                fingerprint = self._fingerprint(port)
                if self.connection is not None and fingerprint != self.fingerprint:
                    self._close()
                if self.connection is None:
                    self.connection = self.backend.open(port.device)
                    self.fingerprint = fingerprint
                    self.connection_id = str(uuid.uuid4())
                self.device = self._status()
                if self.device and self.device["state"] == "configured":
                    identity = (self.device["deviceId"], self.device["credentialVersion"])
                    if identity != self.verified_identity:
                        self.verification = "pending"
                        self.last_verification = 0.0
                        self.verified_identity = identity
                    retry_after = 60 if self.verification == "rate_limit" else 10
                    terminal = self.verification in ("ready", "tls", "credential")
                    if not terminal and (not self.last_verification or
                                         time.monotonic() - self.last_verification >= retry_after):
                        self.last_verification = time.monotonic()
                        self.verification = self._verify(self.device)
                else:
                    self.verification = "pending"
                    self.verified_identity = None
            except Exception:
                self._close()

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            if self.connection is None:
                return {"state": "disconnected"}
            if self.device is None:
                return {"state": "connecting"}
            hardware_id = hmac.new(self.settings.grant_key,
                f"AHSTATION/1/HARDWARE\n{self.settings.station_id}\n{self.fingerprint[1]}".encode("utf-8"),
                hashlib.sha256).hexdigest()
            result = {"connectionId": self.connection_id, "hardwareId": hardware_id, **self.device}
            result["presenceGrant"] = presence_grant(self.settings, self.connection_id, hardware_id,
                self.device["state"], self.device.get("deviceId", "-"))
            if self.device["state"] == "configured":
                result["verification"] = self.verification
                if self.verification == "ready":
                    result["verificationGrant"] = verification_grant(
                        self.settings, self.connection_id, self.device["deviceId"], self.device["deviceNumber"],
                        hardware_id, self.device["credentialVersion"])
            return result

    def provision(self, connection_id: str, device_id: str, number: int, secret: str) -> dict[str, Any]:
        if not valid_identity(device_id, number, secret):
            raise StationError("invalid_request", 400)
        with self.lock:
            if not self.connection or not self.connection_id or connection_id != self.connection_id:
                raise StationError("stale_connection")
            if self.device is None:
                raise StationError("device_not_ready")
            if self.device["state"] != "unconfigured":
                raise StationError("already_configured")
            try:
                port = self._matching_port()
                if port is None or self._fingerprint(port) != self.fingerprint:
                    self._close()
                    raise StationError("stale_connection")
                self.device = self._status()
                if self.device is None:
                    raise StationError("device_not_ready")
                if self.device["state"] != "unconfigured":
                    raise StationError("already_configured")
                port = self._matching_port()
                if port is None or self._fingerprint(port) != self.fingerprint:
                    self._close()
                    raise StationError("stale_connection")
                payload = build_set(self.settings, device_id, number, secret)
                self.connection.reset_input_buffer()
                if self.connection.write(payload) != len(payload):
                    raise StationError("outcome_unknown")
                self.connection.flush()
                line = self._read_response(("AHCFG/1 ACK ", "AHCFG/1 ERROR "), 3.0)
                match = ACK.fullmatch(line or "")
                if match and match[1] == device_id and int(match[2]) == number:
                    self.device = {"state": "configured", "deviceId": device_id,
                                   "deviceNumber": number, "credentialVersion": int(match[3])}
                    self.verification = "pending"
                    self.verified_identity = (device_id, int(match[3]))
                    self.last_verification = 0.0
                    return self.snapshot()
                error = ERROR.fullmatch(line or "")
                if error:
                    self.device = None
                    raise StationError({"INVALID": "invalid_configuration",
                                        "CERTIFICATE": "invalid_certificate",
                                        "STORAGE": "storage_error",
                                        "LOCKED": "already_configured"}.get(error[1], "device_rejected"))
                self.device = None
                raise StationError("outcome_unknown")
            except StationError:
                raise
            except Exception:
                self._close()
                raise StationError("outcome_unknown") from None

    def repair(self, connection_id: str, device_id: str, number: int, secret: str) -> dict[str, Any]:
        if not valid_identity(device_id, number, secret):
            raise StationError("invalid_request", 400)
        with self.lock:
            if not self.connection or connection_id != self.connection_id:
                raise StationError("stale_connection")
            if self.device is None or self.device.get("state") != "configured" or \
                    self.device.get("deviceId") != device_id or self.device.get("deviceNumber") != number:
                raise StationError("identity_mismatch")
            try:
                port = self._matching_port()
                if port is None or self._fingerprint(port) != self.fingerprint:
                    self._close()
                    raise StationError("stale_connection")
                fresh = self._status()
                if fresh is None or fresh.get("state") != "configured" or \
                        fresh.get("deviceId") != device_id or fresh.get("deviceNumber") != number:
                    raise StationError("identity_mismatch")
                port = self._matching_port()
                if port is None or self._fingerprint(port) != self.fingerprint:
                    self._close()
                    raise StationError("stale_connection")
                payload = build_set(self.settings, device_id, number, secret).replace(b"AHCFG/1 SET ", b"AHCFG/1 UPDATE ", 1)
                self.connection.reset_input_buffer()
                if self.connection.write(payload) != len(payload):
                    raise StationError("outcome_unknown")
                self.connection.flush()
                line = self._read_response(("AHCFG/1 ACK ", "AHCFG/1 ERROR "), 3.0)
                updated = ACK_UPDATED.fullmatch(line or "")
                if updated and updated[1] == device_id and int(updated[2]) == number and \
                        int(updated[3]) in (fresh["credentialVersion"], fresh["credentialVersion"] + 1):
                    self.device = {"state": "configured", "deviceId": device_id,
                                   "deviceNumber": number, "credentialVersion": int(updated[3])}
                    self.verification = "pending"
                    self.last_verification = 0.0
                    return self.snapshot()
                error = ERROR.fullmatch(line or "")
                if error:
                    raise StationError({"INVALID": "invalid_configuration",
                                        "CERTIFICATE": "invalid_certificate",
                                        "STORAGE": "storage_error",
                                        "LOCKED": "already_configured"}.get(error[1], "device_rejected"))
                raise StationError("outcome_unknown")
            except StationError:
                raise
            except Exception:
                self._close()
                raise StationError("outcome_unknown") from None

    def close(self) -> None:
        with self.lock:
            self._close()
