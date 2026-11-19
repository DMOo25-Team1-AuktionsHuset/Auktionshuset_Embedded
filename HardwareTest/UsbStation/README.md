# Fixed USB provisioning station

This Windows program bridges the staff registration page to the firmware's
[`AHCFG/1` serial protocol](../AuctionDevice/README.md#one-time-usb-configuration).
It does **not** register a device in the backend. The employee's click on the web
page must cause backend registration first; only that result and a backend-signed
grant can be sent to the local station. One device is connected to the fixed USB
port at a time. The background process stays running when devices are unplugged.

## Install and configure once

1. Install Python 3 on the Windows setup computer and run
   `py -3 -m pip install --user -r HardwareTest/UsbStation/requirements.txt`.
2. Flash the same `AuctionDevice` firmware on all devices. Connect one flashed
   device to the permanent USB cable/port. Its USB descriptor must expose a
   stable, unique serial number and a physical `location`; the station fails
   closed without either. The COM number may change between devices.
3. Store a random 32-byte shared HMAC key in the web backend's secret store.
   Enter its 64-character hex representation at the station's hidden prompt.
   Keep it off the web page and out of logs. The backend grant endpoint
   must use the same key for this station.
4. From the repository root run `py -3 HardwareTest/UsbStation/station.py init`.
   Select the fixed USB location once if more than one eligible serial port is
   present. Enter the exact approved HTTPS web origin, a loopback port (default
   8765), Wi-Fi SSID/password, backend certificate hostname/port, TLS setting
   and PEM trust-anchor file. TLS certificate parsing is checked at setup.
   Use plain WS only in explicitly permitted isolated development.
5. Run `py -3 HardwareTest/UsbStation/station.py run` manually for acceptance.
   Then configure Windows Task Scheduler to start that command at logon under
   the **same Windows account**, with a single running instance and automatic
   restart after failure. The process listens only on `127.0.0.1` and scans the
   configured USB location; staff do not choose a COM port or restart it for
   each device.

Settings live at `%LOCALAPPDATA%\AuktionshusetUsbStation\station.bin`, encrypted
with Windows user DPAPI and replaced atomically. The station ID printed by
`init` is safe to record in the backend. `init` refuses to overwrite settings;
`init --replace` is a deliberate administrator action. Run and reconfigure under
the same Windows account. Replacing station settings affects future devices;
configured devices remain locked in their own NVS.

## Local API for the registration page

The API is HTTP on loopback only. Every request needs the exact approved
`Origin` and `Host: 127.0.0.1:<port>`; other web origins receive no CORS access.
The station also handles the browser's private-network preflight. Requests and
responses use JSON and are never logged. No secret or Wi-Fi password appears in
status, success, error responses or application logs.

| Endpoint | Purpose |
| --- | --- |
| `GET /v1/device` | Returns `protocolVersion`, `stationId`, and `state`: `disconnected`, `connecting`, `unconfigured` or `configured`. Ready devices include an ephemeral `connectionId`; configured devices also include `deviceId` and `deviceNumber`. |
| `POST /v1/provision` | Only after employee registration. JSON body: `connectionId`, `deviceId`, `deviceNumber`, `secret`. Header: `X-Station-Grant`. The station supplies its protected Wi-Fi/backend/TLS settings. Success returns only safe identity and connection fields. |
| `POST /v1/repair` | Explicit staff credential rotation or resumed repair. Same signed request format. Requires the currently configured device ID and number on the same fixed USB connection; sends `AHCFG/1 UPDATE`. |

Connected status includes a stable `hardwareId`, an HMAC of the station ID and USB serial number. It is safe to display and lets the backend bind a pending registration to the same physical device after a reconnect or station restart. A replacement device has a different hardware ID and cannot receive the pending credential through **Genoptag**. The station does not cache or log credentials.

Connected status also includes a 90-second `presenceGrant`. The backend validates its HMAC before registration, resume or credential rotation. The signed ASCII fields are `AHSTATION/1/PRESENT`, station ID, approved origin, expiry, connection ID, hardware ID, state (`unconfigured` or `configured`) and saved device ID (or `-`), joined with newlines. This proves the station observed the current physical USB identity; it cannot be created by changing `hardwareId` in a web request. The station still checks the USB descriptor and serial status immediately before transfer.

For configured devices, `GET /v1/device` also returns `credentialVersion` and `verification`: `pending`, `ready`, `wifi`, `clock`, `backend`, `tls`, `credential` or `rate_limit`. The station sends `AHCFG/1 VERIFY` on the same fixed serial connection and retries transient failures. TLS and credential failures are reported without repeating a permanent credential error or disclosing secrets. When `verification` is `ready`, status also includes a short-lived `verificationGrant` signed by the station. The web page sends it to the staff-only completion endpoint; backend activation requires its signature, station/connection/identity/version binding and expiry. A grant for an earlier credential version cannot complete a rotation. The page displays **Klar** only after that completion succeeds. The firmware contacts the backend directly over the configured network; the station does not stand in for the Wi-Fi or TLS check.

The backend must issue `X-Station-Grant` **after** the employee's registration
action. Its exact format is `v1.<expiryUnixSeconds>.<lowercaseHmacSha256Hex>`.
The HMAC key is the station's 32-byte shared key. The signed message is ASCII,
with `\n` between fields and no trailing newline:

```text
AHSTATION/1
<stationId>
<approvedOrigin>
<expiryUnixSeconds>
<connectionId>
<deviceId>
<deviceNumber>
<lowercaseSha256HexOfThe64CharacterSecret>
```

The grant expires within 120 seconds and binds the registration result to the
specific station and USB connection. The browser obtains `stationId` and
`connectionId` from `GET`, sends them with its registration action to the web
backend, then posts the backend's device ID, number, secret and grant locally.
The page must not cache the secret or retry registration after a serial timeout.
If `POST` reports `outcome_unknown`, query `GET` again and reconcile the backend
registration with the device identity before any further action. The station
never retries a `SET` on a new serial connection.

The station requires a unique USB serial number and the configured physical
location in addition to VID/PID. A disconnect, COM change or serial-number
change invalidates `connectionId`. Before sending `SET`, it checks the current
USB descriptor, asks the device for fresh `AHCFG/1 STATUS UNCONFIGURED`, and
checks the descriptor again. It keeps the same open serial handle through the
write and acknowledgement. Already configured devices are identified safely and
are never overwritten.

## Verification and remaining hardware checks

Run simulated serial, local HTTP security and Windows DPAPI tests from the
repository root:

```powershell
py -3 -B -m unittest discover -s HardwareTest/UsbStation -p test_station.py -v
```

Physical acceptance still needs the actual ESP32-S3, fixed USB cable/hub,
Windows driver and approved browser: confirm stable unique USB serial numbers,
COM renumbering, unplug/replug during `SET`, USB CDC/serial availability, NVS
recovery after power loss, local-network browser permissions, and WSS backend
connectivity. No hardware provisioning or browser-to-backend registration flow
has been run by these simulated tests.
