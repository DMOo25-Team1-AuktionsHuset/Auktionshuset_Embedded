# AuctionDevice

ESP32 sketch based on the `../test5` wiring, with an additional separate bid button.

## Board and libraries

The sketch was compile-tested with **ESP32S3 Dev Module** (`esp32:esp32:esp32s3`) using the default build settings: CPU 240 MHz, flash mode QIO, flash frequency 80 MHz, 4 MB flash and PSRAM disabled. Serial monitor: 115200 baud.

The following versions were used for the verified compile:

| Component | Version |
| --- | --- |
| ESP32 Arduino core | 3.3.11 |
| ArduinoJson | 7.4.2 |
| WebSockets by Markus Sattler | 2.6.1 |
| Adafruit GFX Library | 1.12.4 |
| Adafruit ILI9341 | 1.6.2 |
| Adafruit BusIO | 1.17.4 |

This was a compile test only, not an upload or hardware test. The actual N16R8 device requires verification of its 16 MB flash and PSRAM settings before upload; the 4 MB / PSRAM-disabled compile defaults must not be assumed correct for that device. Confirm the actual hardware and select its matching settings rather than guessing.

| Signal | GPIO |
| --- | ---: |
| TFT MOSI / MISO / SCLK | 11 / 13 / 12 |
| TFT CS / DC / RST | 10 / 9 / 8 |
| Encoder A / B / switch | 4 / 5 / 6 |
| Separate bid button | 7 |

Connect the bid button between GPIO 7 and GND. Inputs use internal pull-ups. The encoder uses the same four-transition direction as test5; the switch debounce is 40 ms. The agreed device contract uses the highest bid, otherwise a positive starting price, as reference. A new lot starts at its starting price and a higher bid raises the draft as needed. One detent changes the draft by 1% of the reference rounded to whole kroner, at least 1 kr, and clamps it at the reference. Pressing the encoder **only changes the draft** to 110% of the reference, rounded to kroner; when necessary this is raised to at least 1 kr above the highest bid. Rounding is away from zero at midpoints. Zero starting price with no highest bid disables the draft. A separate 40 ms debounced bid button sends at most once per stable press; the backend remains authoritative. These device rules are not minimum increments for web bids.

## Configuration and security

Copy `config.h.example` to `config.h`, then supply the actual Wi-Fi credentials, backend host, port, provisioned device UUID and credential. `config.h` is ignored by Git; never commit secrets. The endpoint is `/ws/device`. Default TLS is enabled and requires `DEVICE_TLS_CA_CERT` to contain the PEM trust anchor for the backend certificate. Use the certificate hostname as `DEVICE_BACKEND_HOST` and configure a correct system clock before connecting; certificate validity and hostname checks depend on the ESP32 TLS stack. Provision and rotate the trust anchor securely. Prefer **WSS with certificate and hostname validation** for credentials and bids; never silently downgrade or disable validation. Plain WS exposes secrets and should only be considered for explicitly permitted isolated development. The server may not accept remote plain WS.

## Interface and limitations

The sketch uses `DeviceProtocol` for network state, bid correlation, identical-payload retries and receipts. The screen shows the selected lot UUID, authoritative highest and customer accepted bids, draft and request status. A receipt does not itself change the accepted bid display. Use the following serial commands at 115200 baud with a newline for manual reconciliation and receipt handling.

- After every startup, inspect the backend for any bid that might have been sent before reset, reconcile its outcome, then send `RECONCILED`. This enables bidding; it must not be used as a substitute for checking the backend. The module keeps pending requests only in RAM, so a reset loses the request ID and payload. Reconciliation after power loss may require operator investigation by device, customer and time.
- With a synchronized open lot, a positive reference and no unresolved request, press the separate GPIO 7 button to submit a positive draft. Encoder press never transmits; there is no serial `BID` command. The serial log reports the request ID and pending/final outcome; a send failure with a retained request is **unknown**, not a rejection.
- If a receipt is missing or the outcome is unknown, `RETRY` resends the original request and payload, only if the module has re-synchronized in the same session. It does not create a new bid. There is no automatic retry or receipt timeout. If retry is impossible or an unknown outcome persists, reconcile externally; only then may `RECONCILED` clear an unknown request. A pending request cannot be cleared this way.
- Record an accepted or rejected receipt and its request ID before sending `ACK`, which clears that final receipt and permits another bid. The receipt is not an auction state update; only the authoritative snapshot changes displayed accepted bids.

A zero draft can result from rounding 110% of a 1–45 øre starting price without a highest bid. It is displayed as 0.00 and cannot be submitted; rotate the encoder to adjust it. Arithmetic uses unsigned 64-bit øre; a movement or encoder press whose exact result cannot fit is ignored. The protocol accepts bids up to INT64_MAX øre, so a larger draft is shown but cannot be submitted. Incoming state amounts can be larger than that limit.

Do not reset to bypass pending/unknown outcomes. Socket, protocol and receipt handling remain subject to hardware and end-to-end verification. WSS validation has not been tested against a live server.

Copy the config and install the listed dependencies to compile `AuctionDevice.ino`. Before uploading, verify the actual board, flash and PSRAM settings. Hardware behavior, protocol interoperability and WSS verification have not been tested on the device.
