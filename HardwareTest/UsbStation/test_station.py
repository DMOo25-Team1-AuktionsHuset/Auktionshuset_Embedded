from __future__ import annotations

import hashlib
import hmac
import http.client
import json
import os
import tempfile
import threading
import time
import unittest
from collections import deque
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

from station_config import DPAPIProtector, load, save, validate
from station_core import Settings, Station, StationError, grant_message, verify_grant, verification_grant, presence_grant
from station_server import create_server


DEVICE_ID = "11111111-1111-4111-8111-111111111111"
OTHER_ID = "22222222-2222-4222-8222-222222222222"
SECRET = "A" * 64


def settings() -> Settings:
    return Settings("33333333-3333-4333-8333-333333333333", "https://staff.example.test", 8765,
                    0x303A, 0x1001, "1-4", "AuctionWifi", "private-password",
                    "backend.example.test", 443, False, "", b"K" * 32)


def grant(config: Settings, connection_id: str, device_id: str = DEVICE_ID,
          number: int = 42, secret: str = SECRET, expires: int | None = None) -> str:
    expiry = expires if expires is not None else int(time.time()) + 60
    digest = hmac.new(config.grant_key,
                      grant_message(config, expiry, connection_id, device_id, number, secret),
                      hashlib.sha256).hexdigest()
    return f"v1.{expiry}.{digest}"


class FakeDevice:
    def __init__(self, serial_number: str, configured: bool = False):
        self.serial_number = serial_number
        self.configured = configured
        self.device_id = DEVICE_ID
        self.number = 42
        self.secret = SECRET
        self.version = 1
        self.set_commands: list[bytes] = []
        self.update_commands: list[bytes] = []
        self.ack = True
        self.trailing_ack = False
        self.verification = "READY"
        self.reject_code: str | None = None
        self.verify_commands = 0


class FakeSerial:
    def __init__(self, device: FakeDevice, backend: "FakeBackend"):
        self.device = device
        self.backend = backend
        self.closed = False
        self.responses: deque[bytes] = deque()

    def write(self, data: bytes) -> int:
        if self.closed:
            raise OSError("disconnected")
        if data == b"AHCFG/1 STATUS\n":
            self.responses.append(b"boot log without credentials\n")
            if self.device.configured:
                self.responses.append(f"AHCFG/1 STATUS CONFIGURED {self.device.device_id} {self.device.number} {self.device.version}\n".encode())
            else:
                self.responses.append(b"AHCFG/1 STATUS UNCONFIGURED\n")
                if self.device.trailing_ack:
                    self.responses.append(f"AHCFG/1 ACK CONFIGURED {DEVICE_ID} 42 1\n".encode())
            if self.backend.switch_after_status is not None:
                self.backend.active = self.backend.switch_after_status
                self.backend.switch_after_status = None
        elif data == b"AHCFG/1 VERIFY\n":
            self.device.verify_commands += 1
            if self.device.verification == "READY":
                self.responses.append(f"AHCFG/1 VERIFY READY {self.device.device_id} {self.device.number} {self.device.version}\n".encode())
            else:
                self.responses.append(f"AHCFG/1 VERIFY ERROR {self.device.verification}\n".encode())
        elif data.startswith(b"AHCFG/1 SET "):
            self.device.set_commands.append(data)
            if self.device.reject_code is not None:
                self.responses.append(f"AHCFG/1 ERROR {self.device.reject_code}\n".encode())
                return len(data)
            parts = data.decode().strip().split(" ")
            self.device.configured = True
            self.device.device_id = parts[2]
            self.device.number = int(parts[3])
            self.device.secret = parts[4]
            if self.device.ack:
                self.responses.append(f"AHCFG/1 ACK CONFIGURED {self.device.device_id} {self.device.number} {self.device.version}\n".encode())
        elif data.startswith(b"AHCFG/1 UPDATE "):
            self.device.update_commands.append(data)
            new_secret = data.decode().strip().split(" ")[4]
            if new_secret != self.device.secret:
                self.device.version += 1
                self.device.secret = new_secret
            if self.device.ack:
                self.responses.append(f"AHCFG/1 ACK UPDATED {self.device.device_id} {self.device.number} {self.device.version}\n".encode())
        return len(data)

    def flush(self) -> None:
        if self.closed:
            raise OSError("disconnected")

    def reset_input_buffer(self) -> None:
        if self.closed:
            raise OSError("disconnected")
        self.responses.clear()

    def readline(self, _limit: int) -> bytes:
        if self.closed:
            raise OSError("disconnected")
        return self.responses.popleft() if self.responses else b""

    def close(self) -> None:
        self.closed = True


class FakeBackend:
    def __init__(self, active: FakeDevice | None = None):
        self.active = active
        self.switch_after_status: FakeDevice | None = None
        self.opened: list[FakeSerial] = []

    def ports(self):
        if self.active is None:
            return []
        return [SimpleNamespace(device="COM9", vid=0x303A, pid=0x1001,
                                location="1-4", serial_number=self.active.serial_number)]

    def open(self, _device: str):
        serial = FakeSerial(self.active, self)
        self.opened.append(serial)
        return serial


class StationSessionTests(unittest.TestCase):
    def setUp(self):
        self.config = settings()
        self.first = FakeDevice("unit-a")
        self.backend = FakeBackend(self.first)
        self.station = Station(self.config, self.backend)

    def tearDown(self):
        self.station.close()

    def test_fixed_port_reconnect_and_no_automatic_registration(self):
        self.assertEqual(self.station.snapshot(), {"state": "disconnected"})
        self.station.poll()
        first_id = self.station.snapshot()["connectionId"]
        self.assertEqual(self.station.snapshot()["state"], "unconfigured")
        self.assertEqual(self.first.set_commands, [])
        self.backend.active = None
        self.station.poll()
        self.assertEqual(self.station.snapshot(), {"state": "disconnected"})
        second = FakeDevice("unit-b", configured=True)
        second.device_id = OTHER_ID
        self.backend.active = second
        self.station.poll()
        self.assertNotEqual(self.station.snapshot()["connectionId"], first_id)
        self.assertEqual(self.station.snapshot()["deviceId"], OTHER_ID)
        self.assertEqual(second.set_commands, [])

    def test_device_swap_before_provision_rejects_old_connection(self):
        self.station.poll()
        old_connection = self.station.snapshot()["connectionId"]
        replacement = FakeDevice("unit-b")
        self.backend.active = replacement
        with self.assertRaisesRegex(StationError, "stale_connection"):
            self.station.provision(old_connection, DEVICE_ID, 42, SECRET)
        self.assertEqual(self.first.set_commands, [])
        self.assertEqual(replacement.set_commands, [])
        self.station.poll()
        self.assertNotEqual(self.station.snapshot()["connectionId"], old_connection)

    def test_device_swap_after_status_probe_rejects_before_set(self):
        self.station.poll()
        old_connection = self.station.snapshot()["connectionId"]
        replacement = FakeDevice("unit-b")
        self.backend.switch_after_status = replacement
        with self.assertRaisesRegex(StationError, "stale_connection"):
            self.station.provision(old_connection, DEVICE_ID, 42, SECRET)
        self.assertEqual(self.first.set_commands, [])
        self.assertEqual(replacement.set_commands, [])

    def test_provision_once_and_safe_identity(self):
        self.station.poll()
        connection_id = self.station.snapshot()["connectionId"]
        outcome = self.station.provision(connection_id, DEVICE_ID, 42, SECRET)
        self.assertEqual(outcome["state"], "configured")
        self.assertEqual(outcome["deviceId"], DEVICE_ID)
        self.assertEqual(len(self.first.set_commands), 1)
        self.assertIn(SECRET.encode(), self.first.set_commands[0])
        self.assertNotIn(SECRET, json.dumps(outcome))
        self.assertNotIn(self.config.wifi_password, json.dumps(outcome))
        with self.assertRaisesRegex(StationError, "already_configured"):
            self.station.provision(connection_id, OTHER_ID, 43, SECRET)
        self.assertEqual(len(self.first.set_commands), 1)

    def test_device_rejection_exposes_only_safe_error_code(self):
        self.first.reject_code = "INVALID"
        self.station.poll()
        connection_id = self.station.snapshot()["connectionId"]
        with self.assertRaisesRegex(StationError, "invalid_configuration"):
            self.station.provision(connection_id, DEVICE_ID, 42, SECRET)
        self.assertEqual(self.station.snapshot().get("state"), "connecting")
        self.assertNotIn(SECRET, str(self.station.snapshot()))
        self.first.reject_code = "CERTIFICATE"
        self.station.poll()
        with self.assertRaisesRegex(StationError, "invalid_certificate"):
            self.station.provision(connection_id, DEVICE_ID, 42, SECRET)
        self.assertNotIn(self.config.wifi_password, str(self.station.snapshot()))

    def test_verification_without_customer_data_and_tls_failure(self):
        self.first.configured = True
        self.first.verification = "TLS"
        self.station.poll()
        outcome = self.station.snapshot()
        self.assertEqual(outcome["verification"], "tls")
        self.assertEqual(self.first.verify_commands, 1)
        self.assertNotIn(SECRET, json.dumps(outcome))
        self.assertNotIn(self.config.wifi_password, json.dumps(outcome))

    def test_credential_failure_is_reported_without_secrets(self):
        self.first.configured = True
        self.first.verification = "CREDENTIAL"
        self.station.poll()
        self.assertEqual(self.station.snapshot()["verification"], "credential")
        self.station.poll()
        self.assertEqual(self.first.verify_commands, 1)

    def test_successful_verification_grant_binds_connection_and_identity(self):
        self.first.configured = True
        self.station.poll()
        outcome = self.station.snapshot()
        self.assertEqual(outcome["verification"], "ready")
        signed = outcome["verificationGrant"]
        self.assertEqual(signed, verification_grant(self.config, outcome["connectionId"], DEVICE_ID, 42,
                                                   outcome["hardwareId"], outcome["credentialVersion"],
                                                   int(signed.split(".")[1]) - 90))
        self.assertNotIn(SECRET, json.dumps(outcome))
        self.assertNotIn(self.config.wifi_password, json.dumps(outcome))

    def test_repair_requires_same_connection_and_identity(self):
        self.first.configured = True
        self.station.poll()
        connection_id = self.station.snapshot()["connectionId"]
        with self.assertRaisesRegex(StationError, "identity_mismatch"):
            self.station.repair(connection_id, OTHER_ID, 42, SECRET)
        replacement = FakeDevice("unit-b", configured=True)
        self.backend.active = replacement
        with self.assertRaisesRegex(StationError, "stale_connection"):
            self.station.repair(connection_id, DEVICE_ID, 42, SECRET)
        self.assertEqual(replacement.update_commands, [])

    def test_device_swap_during_repair_probe_does_not_receive_update(self):
        self.first.configured = True
        self.station.poll()
        old_connection = self.station.snapshot()["connectionId"]
        replacement = FakeDevice("unit-b", configured=True)
        self.backend.switch_after_status = replacement
        with self.assertRaisesRegex(StationError, "stale_connection"):
            self.station.repair(old_connection, DEVICE_ID, 42, SECRET)
        self.assertEqual(self.first.update_commands, [])
        self.assertEqual(replacement.update_commands, [])

    def test_station_restart_keeps_hardware_identity_without_reprovisioning(self):
        self.first.configured = True
        self.station.poll()
        before = self.station.snapshot()
        self.station.close()
        restarted = Station(self.config, self.backend)
        try:
            restarted.poll()
            after = restarted.snapshot()
            self.assertNotEqual(before["connectionId"], after["connectionId"])
            self.assertEqual(before["hardwareId"], after["hardwareId"])
            self.assertEqual(after["deviceNumber"], 42)
            self.assertEqual(self.first.set_commands, [])
        finally:
            restarted.close()

    def test_presence_proof_binds_connection_hardware_and_saved_identity(self):
        self.station.poll()
        first = self.station.snapshot()
        expires = int(first["presenceGrant"].split(".")[1])
        self.assertEqual(first["presenceGrant"], presence_grant(self.config,
            first["connectionId"], first["hardwareId"], "unconfigured", now=expires - 90))
        replacement = FakeDevice("unit-b", configured=True)
        replacement.device_id = OTHER_ID
        self.backend.active = replacement
        self.station.poll()
        second = self.station.snapshot()
        self.assertNotEqual(first["hardwareId"], second["hardwareId"])
        self.assertNotEqual(first["presenceGrant"], second["presenceGrant"])
        self.assertEqual(second["presenceGrant"], presence_grant(self.config,
            second["connectionId"], second["hardwareId"], "configured", OTHER_ID,
            now=int(second["presenceGrant"].split(".")[1]) - 90))

    def test_repair_lost_ack_keeps_single_explicit_update(self):
        self.first.configured = True
        self.station.poll()
        connection_id = self.station.snapshot()["connectionId"]
        self.first.ack = False
        with self.assertRaisesRegex(StationError, "outcome_unknown"):
            self.station.repair(connection_id, DEVICE_ID, 42, "B" * 64)
        self.assertEqual(len(self.first.update_commands), 1)
        self.station.poll()
        self.assertEqual(self.station.snapshot()["state"], "configured")
        self.assertEqual(self.station.snapshot()["credentialVersion"], 2)
        self.assertEqual(len(self.first.update_commands), 1)
        self.first.ack = True
        self.station.repair(connection_id, DEVICE_ID, 42, "B" * 64)
        self.assertEqual(self.station.snapshot()["credentialVersion"], 2)

    def test_lost_ack_does_not_resend_or_rebind(self):
        self.station.poll()
        connection_id = self.station.snapshot()["connectionId"]
        self.first.ack = False
        with self.assertRaisesRegex(StationError, "outcome_unknown"):
            self.station.provision(connection_id, DEVICE_ID, 42, SECRET)
        self.assertEqual(len(self.first.set_commands), 1)
        self.station.poll()
        self.assertEqual(self.station.snapshot()["state"], "configured")
        self.assertEqual(len(self.first.set_commands), 1)

    def test_stale_ack_is_discarded_before_set(self):
        self.station.poll()
        self.first.ack = False
        self.first.trailing_ack = True
        with self.assertRaisesRegex(StationError, "outcome_unknown"):
            self.station.provision(self.station.snapshot()["connectionId"], DEVICE_ID, 42, SECRET)
        self.assertEqual(len(self.first.set_commands), 1)

    def test_grant_is_short_lived_and_bound_to_identity_secret_and_connection(self):
        self.station.poll()
        connection_id = self.station.snapshot()["connectionId"]
        signed = grant(self.config, connection_id)
        self.assertTrue(verify_grant(self.config, signed, connection_id, DEVICE_ID, 42, SECRET))
        self.assertFalse(verify_grant(self.config, signed, "new-connection", DEVICE_ID, 42, SECRET))
        self.assertFalse(verify_grant(self.config, signed, connection_id, OTHER_ID, 42, SECRET))
        self.assertFalse(verify_grant(self.config, signed, connection_id, DEVICE_ID, 42, "B" * 64))
        self.assertFalse(verify_grant(self.config, grant(self.config, connection_id, expires=1),
                                      connection_id, DEVICE_ID, 42, SECRET))


class LocalApiTests(unittest.TestCase):
    def setUp(self):
        self.config = settings()
        self.device = FakeDevice("unit-a")
        self.station = Station(self.config, FakeBackend(self.device))
        self.station.poll()
        self.server = create_server(self.station, port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.station.close()

    def request(self, method: str, path: str, body: object | None = None,
                origin: str | None = "https://staff.example.test", extra: dict | None = None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=5)
        headers = {} if origin is None else {"Origin": origin}
        if extra:
            headers.update(extra)
        payload = None if body is None else json.dumps(body).encode()
        if payload is not None:
            headers["Content-Type"] = "application/json"
        connection.request(method, path, body=payload, headers=headers)
        response = connection.getresponse()
        data = response.read()
        result = response.status, dict(response.getheaders()), data
        connection.close()
        return result

    def test_origin_host_preflight_and_grant(self):
        status, headers, body = self.request("GET", "/v1/device", origin="https://evil.example.test")
        self.assertEqual(status, 403)
        self.assertNotIn("Access-Control-Allow-Origin", headers)
        status, _, _ = self.request("GET", "/v1/device", extra={"Host": "evil.example.test"})
        self.assertEqual(status, 403)
        status, headers, body = self.request("GET", "/v1/device")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Access-Control-Allow-Origin"], self.config.approved_origin)
        self.assertEqual(json.loads(body)["stationId"], self.config.station_id)
        self.assertNotIn(SECRET.encode(), body)
        self.assertNotIn(self.config.wifi_password.encode(), body)
        connection_id = json.loads(body)["connectionId"]
        status, _, _ = self.request("OPTIONS", "/v1/provision", extra={
            "Access-Control-Request-Method": "POST",
            "Access-Control-Request-Headers": "content-type,x-station-grant",
            "Access-Control-Request-Private-Network": "true"})
        self.assertEqual(status, 204)
        payload = {"connectionId": connection_id, "deviceId": DEVICE_ID,
                   "deviceNumber": 42, "secret": SECRET}
        status, _, body = self.request("POST", "/v1/provision", payload)
        self.assertEqual(status, 403)
        self.assertEqual(self.device.set_commands, [])
        status, _, body = self.request("POST", "/v1/provision", payload, extra={
            "X-Station-Grant": grant(self.config, connection_id)})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["state"], "configured")
        self.assertEqual(len(self.device.set_commands), 1)
        self.assertNotIn(SECRET.encode(), body)
        self.assertNotIn(self.config.wifi_password.encode(), body)


class ProtectedSettingsTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows DPAPI only")
    def test_dpapi_round_trip(self):
        protector = DPAPIProtector()
        payload = b"test private station settings"
        self.assertEqual(protector.unprotect(protector.protect(payload)), payload)

    def test_settings_validation(self):
        validate(settings())
        with self.assertRaises(ValueError):
            validate(replace(settings(), approved_origin="http://staff.example.test"))
        with self.assertRaises(ValueError):
            validate(replace(settings(), grant_key=b"short"))
        with self.assertRaises(ValueError):
            validate(replace(settings(), tls=True, ca_pem="invalid"))

    def test_encrypted_file_and_round_trip(self):
        class Protector:
            def protect(self, value: bytes) -> bytes:
                return bytes(byte ^ 0xA5 for byte in value)

            def unprotect(self, value: bytes) -> bytes:
                return self.protect(value)

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "station.bin"
            protector = Protector()
            save(settings(), path, protector)
            raw = path.read_bytes()
            self.assertNotIn(settings().wifi_password.encode(), raw)
            self.assertNotIn(settings().grant_key, raw)
            self.assertEqual(load(path, protector), settings())
            with self.assertRaises(FileExistsError):
                save(settings(), path, protector)


if __name__ == "__main__":
    unittest.main()
