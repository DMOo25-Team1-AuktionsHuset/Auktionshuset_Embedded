"""Small loopback-only API for the approved registration page."""

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from station_core import UUID, Station, StationError, valid_identity, verify_grant


def create_server(station: Station, port: int | None = None) -> ThreadingHTTPServer:
    listen_port = station.settings.listen_port if port is None else port

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self) -> None:
            super().setup()
            self.connection.settimeout(5)

        def log_message(self, _format: str, *args: Any) -> None:
            # HTTP request bodies and authorization headers must never reach logs.
            pass

        def allowed(self) -> bool:
            return (self.headers.get("Origin") == station.settings.approved_origin
                    and self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}")

        def respond(self, status: int, body: dict[str, Any] | None = None,
                    cors: bool = True) -> None:
            payload = b"" if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            if body is not None:
                self.send_header("Content-Type", "application/json; charset=utf-8")
            if cors:
                self.send_header("Access-Control-Allow-Origin", station.settings.approved_origin)
                self.send_header("Vary", "Origin")
            self.end_headers()
            if payload:
                self.wfile.write(payload)

        def do_OPTIONS(self) -> None:
            if not self.allowed() or self.path not in ("/v1/device", "/v1/provision", "/v1/repair"):
                self.respond(403, {"error": "forbidden"}, cors=False)
                return
            requested_method = self.headers.get("Access-Control-Request-Method", "")
            expected_method = "GET" if self.path == "/v1/device" else "POST"
            requested_headers = {item.strip().lower() for item in
                                 self.headers.get("Access-Control-Request-Headers", "").split(",") if item.strip()}
            if requested_method != expected_method or not requested_headers <= {"content-type", "x-station-grant"}:
                self.respond(403, {"error": "forbidden"}, cors=False)
                return
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", station.settings.approved_origin)
            self.send_header("Access-Control-Allow-Methods", expected_method)
            self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Station-Grant")
            self.send_header("Access-Control-Allow-Private-Network", "true")
            self.send_header("Vary", "Origin")
            self.end_headers()

        def do_GET(self) -> None:
            if not self.allowed():
                self.respond(403, {"error": "forbidden"}, cors=False)
            elif self.path != "/v1/device":
                self.respond(404, {"error": "not_found"})
            else:
                self.respond(200, {"protocolVersion": 1, "stationId": station.settings.station_id,
                                   **station.snapshot()})

        def do_POST(self) -> None:
            if not self.allowed():
                self.respond(403, {"error": "forbidden"}, cors=False)
                return
            if self.path not in ("/v1/provision", "/v1/repair"):
                self.respond(404, {"error": "not_found"})
                return
            if self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "application/json":
                self.respond(415, {"error": "invalid_request"})
                return
            length = self.headers.get("Content-Length", "")
            if not length.isdecimal() or not 0 < int(length) <= 2048:
                self.respond(400, {"error": "invalid_request"})
                return
            try:
                request = json.loads(self.rfile.read(int(length)))
                if not isinstance(request, dict) or set(request) != {
                    "connectionId", "deviceId", "deviceNumber", "secret"
                }:
                    raise ValueError()
                connection_id = request["connectionId"]
                device_id = request["deviceId"]
                number = request["deviceNumber"]
                secret = request["secret"]
                if (not isinstance(connection_id, str) or UUID.fullmatch(connection_id) is None
                        or not valid_identity(device_id, number, secret)):
                    raise ValueError()
            except (OSError, UnicodeError, ValueError, TypeError):
                self.respond(400, {"error": "invalid_request"})
                return
            if not verify_grant(station.settings, self.headers.get("X-Station-Grant"),
                                connection_id, device_id, number, secret):
                self.respond(403, {"error": "forbidden"})
                return
            try:
                outcome = (station.provision(connection_id, device_id, number, secret)
                           if self.path == "/v1/provision" else
                           station.repair(connection_id, device_id, number, secret))
                self.respond(200, {"protocolVersion": 1, "stationId": station.settings.station_id,
                                   **outcome})
            except StationError as exc:
                self.respond(exc.http_status, {"error": exc.code})

    server = ThreadingHTTPServer(("127.0.0.1", listen_port), Handler)
    server.daemon_threads = True
    return server


def run(station: Station) -> None:
    server = create_server(station)
    stop = threading.Event()

    def watch() -> None:
        while not stop.is_set():
            station.poll()
            stop.wait(0.5)

    watcher = threading.Thread(target=watch, name="usb-station-watch", daemon=True)
    watcher.start()
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        stop.set()
        server.server_close()
        station.close()
        watcher.join(timeout=2)
