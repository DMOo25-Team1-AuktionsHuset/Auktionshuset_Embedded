"""Windows USB provisioning station. Run `py -3 station.py init` once, then `run`."""

from __future__ import annotations

import argparse
import getpass
import sys
import uuid
from pathlib import Path

from station_config import DPAPIProtector, default_path, load, save
from station_core import PySerialBackend, Settings, Station
from station_server import run


def _single_station_port():
    ports = [port for port in PySerialBackend().ports()
             if port.vid is not None and port.pid is not None
             and port.location and port.serial_number]
    if not ports:
        raise RuntimeError("Connect a flashed device to the fixed USB port before initial setup")
    for index, port in enumerate(ports, 1):
        print(f"{index}: {port.device}, VID:PID {port.vid:04X}:{port.pid:04X}, location {port.location}")
    if len(ports) == 1:
        return ports[0]
    selection = int(input("Select the fixed USB port once (number): "))
    if not 1 <= selection <= len(ports):
        raise ValueError()
    return ports[selection - 1]


def initialize(path: Path, replace: bool) -> None:
    if path.exists() and not replace:
        raise FileExistsError("Station settings already exist; use init --replace deliberately")
    port = _single_station_port()
    origin = input("Approved HTTPS web origin (no trailing slash): ").strip()
    listen_port = int(input("Loopback HTTP port [8765]: ").strip() or "8765")
    ssid = input("Station Wi-Fi SSID: ")
    password = getpass.getpass("Station Wi-Fi password (not echoed): ")
    host = input("Backend certificate hostname or IPv4 address: ").strip()
    backend_port = int(input("Backend port [443]: ").strip() or "443")
    tls = (input("Use verified TLS? [Y/n]: ").strip().lower() or "y") == "y"
    ca_pem = Path(input("PEM CA certificate file: ").strip()).read_text(encoding="ascii") if tls else ""
    grant_key = bytes.fromhex(getpass.getpass("Backend/station shared HMAC key (hex, not echoed): ").strip())
    settings = Settings(str(uuid.uuid4()), origin, listen_port, port.vid, port.pid,
                        port.location, ssid, password, host, backend_port, tls, ca_pem,
                        grant_key)
    save(settings, path, DPAPIProtector(), replace)
    print(f"Station configured. Station ID: {settings.station_id}. Local API: http://127.0.0.1:{listen_port}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Auktionshuset fixed-port USB station")
    parser.add_argument("--config", type=Path, default=None,
                        help="DPAPI-protected station file; defaults to LOCALAPPDATA")
    actions = parser.add_subparsers(dest="action", required=True)
    init = actions.add_parser("init", help="Configure the fixed USB port and station settings once")
    init.add_argument("--replace", action="store_true", help="Deliberately replace station settings")
    actions.add_parser("run", help="Run the background loopback service")
    args = parser.parse_args()
    try:
        path = args.config or default_path()
        if args.action == "init":
            initialize(path, args.replace)
        else:
            import serial  # Fail at startup if pyserial is missing.
            del serial
            settings = load(path, DPAPIProtector())
            run(Station(settings))
        return 0
    except KeyboardInterrupt:
        return 0
    except Exception:
        # Never include exception text: library errors may contain connection data.
        print("Station setup or startup failed. Check installation, USB identity and protected settings.",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
