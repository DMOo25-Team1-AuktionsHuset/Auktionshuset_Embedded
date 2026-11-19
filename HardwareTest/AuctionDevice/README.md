# AuctionDevice

ESP32 sketch based on the `../test5` wiring, with an additional separate bid button.

## Board and libraries

The sketch was compile-tested with **ESP32S3 Dev Module** (`esp32:esp32:esp32s3`) using the default build settings: CPU 240 MHz, flash mode QIO, flash frequency 80 MHz, 4 MB flash and PSRAM disabled. Serial monitor: 115200 baud.

The following versions were used for the verified compile:

| Component | Version |
| --- | --- |
| ESP32 Arduino core | 3.3.12 |
| ArduinoJson | 7.4.2 |
| WebSockets by Markus Sattler | 2.7.2 |
| Adafruit GFX Library | 1.12.6 |
| Adafruit ILI9341 | 1.6.4 |
| Adafruit BusIO | 1.17.4 |

This was a compile test only, not an upload or hardware test. The actual N16R8 device requires verification of its 16 MB flash and PSRAM settings before upload; the 4 MB / PSRAM-disabled compile defaults must not be assumed correct for that device. Confirm the actual hardware and select its matching settings rather than guessing.

| Signal | GPIO |
| --- | ---: |
| TFT MOSI / MISO / SCLK | 11 / 13 / 12 |
| TFT CS / DC / RST | 10 / 9 / 8 |
| Encoder A / B / switch | 4 / 5 / 6 |
| Separate bid button | 7 |

Connect the bid button between GPIO 7 and GND. Inputs use internal pull-ups. The encoder uses the same four-transition direction as test5; the switch debounce is 40 ms. The agreed device contract uses the highest bid, otherwise a positive starting price, as reference. A new lot starts at its starting price and a higher bid raises the draft as needed. One detent changes the draft by 1% of the reference rounded to whole kroner, at least 1 kr, and clamps it at the reference. Pressing the encoder **only changes the draft** to 110% of the reference, rounded to kroner; when necessary this is raised to at least 1 kr above the highest bid. Rounding is away from zero at midpoints. Zero starting price with no highest bid disables the draft. A separate 40 ms debounced bid button sends at most once per stable press; the backend remains authoritative. These device rules are not minimum increments for web bids.

## One-time USB configuration

The same sketch is flashed to every device. It contains no device-specific `config.h`. Identity, device number, credential, Wi-Fi settings, backend host/port and TLS trust anchor are stored as one versioned, checksummed record in ESP32 Preferences/NVS. An empty or invalid record leaves the device on a USB setup screen; it does not start Wi-Fi. A complete record is activated only after validation, NVS write and read-back. A partial serial frame or NVS blob is never activated. Once configured, ordinary USB connections and subsequent setup commands cannot change it. A deliberate re-provision requires a separate NVS erase through the board tooling; this firmware provides no remote or serial erase command.

Use the board's USB serial connection at **115200 baud** and send one ASCII command per line (`\n`). Protocol version 1 is:

| Host command | Device response |
| --- | --- |
| `AHCFG/1 STATUS` | `AHCFG/1 STATUS UNCONFIGURED` or `AHCFG/1 STATUS CONFIGURED <deviceId> <deviceNumber> <credentialVersion>` |
| `AHCFG/1 SET <deviceId> <deviceNumber> <secret> <ssidHex> <passwordHex> <hostHex> <port> <tls> <caPemHex>` | `AHCFG/1 ACK CONFIGURED <deviceId> <deviceNumber> <credentialVersion>` on success; otherwise `AHCFG/1 ERROR <code>` |
| `AHCFG/1 VERIFY` | Checks Wi-Fi, network time, backend TCP, TLS CA/hostname and `POST /api/device/verify-setup` with the saved device credential. Returns `AHCFG/1 VERIFY READY <deviceId> <deviceNumber> <credentialVersion>` or `AHCFG/1 VERIFY ERROR <code>`. No customer session is needed. |
| `AHCFG/1 UPDATE <same nine fields as SET>` | Explicitly replaces configuration for the same device ID and number; returns `AHCFG/1 ACK UPDATED <deviceId> <deviceNumber> <credentialVersion>` or an error. Ordinary `SET` remains locked. |

`deviceId` is the lowercase canonical nonzero UUID returned by backend registration; `secret` is its 64-character hex credential. `deviceNumber` is a positive decimal integer. Encode the UTF-8 Wi-Fi SSID, password, DNS host (or IPv4 address), and PEM certificate bytes as uppercase or lowercase hex without separators. Limits before hex encoding are 32 bytes for SSID, 64 for password, 253 for host and 3071 for PEM. The password must be empty, 8–63 bytes or a 64-character hex key. Use `-` for an empty Wi-Fi password or CA certificate. `port` is 1–65535 and `tls` is `1` or `0`. With TLS, a parseable PEM CA certificate is required; with plain WS, the CA field must be `-`. The backend path is fixed at `/ws/device`. Keep the certificate hostname as the backend host and ensure the device clock is correct before TLS connections. Plain WS sends credentials without transport protection and is only suitable for explicitly permitted isolated development.

Responses never contain the secret or Wi-Fi password. `ERROR` codes are `INVALID`, `STORAGE`, `LOCKED`, `TOO_LONG`, `TIMEOUT`, `VERSION` or `COMMAND`; invalid frames are not echoed. A line is limited to 8191 bytes and incomplete lines expire after five seconds. If the USB connection ends before a complete line, reconnect and send a fresh command. After `ERROR STORAGE`, restart the device and check `STATUS` before retrying; an uncertain NVS write blocks further setup during that boot. If acknowledgement is lost after the NVS commit, use `STATUS` to confirm the saved device identity. The staff page can resume a pending transfer or explicitly rotate a lost credential; the device never reveals it.

`VERIFY ERROR` codes are `WIFI` (network unavailable), `CLOCK` (NTP time unavailable), `BACKEND` (TCP/HTTP failure), `TLS` (TLS disabled or CA, certificate or hostname rejected), `CREDENTIAL` (wrong credential or disabled device), `RATE_LIMIT`, and `UNCONFIGURED`. A configured device can retry `VERIFY` without rewriting NVS. The firmware does not print the HTTP body or the credential. Verification requires TLS, even if an isolated development configuration permits plain WS for other purposes. The TLS check uses the stored CA and backend hostname; the ESP32 clock is synchronized through NTP before TLS. Production network policy must allow NTP or supply a trusted time source. Ordinary bidding remains gated by the existing customer assignment and authoritative WebSocket state.

`UPDATE` writes a complete, validated record to the inactive NVS slot. Boot selects the newest valid slot; an interrupted or corrupt update falls back to the previous complete configuration. Repeating an identical `UPDATE` after a lost acknowledgement does not write again. The sketch pauses normal bidding after an update until a power cycle, while `VERIFY` checks the new network and credential. Backend rotation invalidates the old credential immediately, so a failed update requires an explicit repair retry on the same device. Neither a normal USB connection nor a station restart invokes `UPDATE` automatically.
Configured `STATUS`, successful `VERIFY` and acknowledgements append the positive credential version after the device number. Initial configuration has version 1; each complete update advances it. Station and backend bind completion to that version so a previous verification cannot activate a rotated credential.

## Interface and limitations

The sketch uses `DeviceProtocol` for network state, bid correlation, identical-payload retries and receipts. The screen shows the selected lot UUID, authoritative highest and customer accepted bids, draft and request status in whole kroner. A receipt does not itself change the accepted bid display. USB serial is reserved for setup and technical service.

- The complete bid payload, request ID, session, lot and amount are written to alternating checksummed NVS records and read back before possible network transmission. The inactive slot is erased before replacement while the current complete request remains intact. Journal buffers are reused outside the ESP32 loop-task stack. On restart an unfinished request is restored as unknown. A torn write or corrupt journal blocks new bidding.
- With a synchronized open lot and no unresolved request, the separate GPIO 7 bid button submits the displayed draft. Encoder rotation changes the draft; encoder press sets its agreed 110% amount.
- If the outcome is unknown, pressing the bid button retries the exact saved payload only in the same session. On WebSocket authentication, the device also asks backend for a previously recorded result using its original request fields; this lookup does not submit a bid and works after a customer change. A missing result remains unknown and blocks bidding.
- Accepted/rejected receipts are saved before display. Pressing the encoder switch acknowledges a final receipt and durably clears the journal. An unknown request cannot be cleared with a serial command or a button. Staff can inspect its device ID and request ID on the device administration page; if the original session has ended and no result exists, staff can explicitly close it as no bid. A backend idempotency record prevents later copies from becoming bids. The bid button checks backend again when retry in the old session is unavailable.

A zero draft can result from rounding 110% of a 1–45 øre starting price without a highest bid. It is displayed as 0 kr and cannot be submitted; rotate the encoder to adjust it. Arithmetic uses unsigned 64-bit øre; a movement or encoder press whose exact result cannot fit is ignored. The protocol accepts bids up to INT64_MAX øre, so a larger draft is shown but cannot be submitted. Incoming state amounts can be larger than that limit.

Do not reset to bypass pending/unknown outcomes. On 2026-11-11, an ESP32-S3 on COM4 completed an isolated physical test with WSS/TLS, two customers, lot switching, encoder/GPIO7 bidding, a lost receipt followed by full power loss, and recovery of the same accepted request without a duplicate. The display showed the result in whole kroner. A loose GPIO7 wire caused an unplanned test bid, so the controls still need fixed mounting before use.

Install the listed dependencies to compile `AuctionDevice.ino`; no credentials are needed at build time. Before uploading to another board, verify its flash, PSRAM and USB CDC settings. The isolated test did not change the Wi-Fi password to a wrong value and did not test a production TLS endpoint or a final mounted enclosure. Those remain physical acceptance checks for deployment.
