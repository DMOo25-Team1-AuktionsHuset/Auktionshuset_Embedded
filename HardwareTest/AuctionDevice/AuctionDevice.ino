#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <inttypes.h>
#include <string.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "DeviceProvisioning.h"
#include "DeviceProtocol.h"

#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCLK 12
#define TFT_CS 10
#define TFT_DC 9
#define TFT_RST 8
#define ENCODER_A 4
#define ENCODER_B 5
#define ENCODER_SW 6
#define BID_BUTTON 7

Adafruit_ILI9341 tft(&SPI, TFT_DC, TFT_CS, TFT_RST);
DeviceProtocol protocol;
DeviceProvisioning provisioning;
uint64_t draft = 0;
String draftAuction, draftLot;
int encoderState, transitions = 0;
bool lastReading, buttonState, lastBidReading, bidButtonState;
unsigned long buttonChanged = 0, bidButtonChanged = 0, lastDraw = 0;
DeviceProtocol::Outcome lastOutcome = DeviceProtocol::Outcome::None;
String lastRequest;
char serialCommand[8192];
size_t serialLength = 0;
bool serialOverflow = false;
unsigned long serialChanged = 0;
const char* setupVerification = "PENDING";
bool repairAwaitingRestart = false;
bool protocolStarted = false;

void startProtocol()
{
    const auto &s = provisioning.settings();
    const DeviceProtocol::Config config = { s.wifiSsid, s.wifiPassword, s.backendHost,
        s.backendPort, "/ws/device", s.deviceId, s.secret, s.tls != 0, s.caCert };
    protocolStarted = protocol.begin(config);
    if (!protocolStarted) Serial.println("Device configuration or bid journal could not start.");
}

// A credential-only check. No session token, customer data or bid channel is used.
// The CA and hostname are checked by WiFiClientSecure before the HTTP request.
const char* verifySetup()
{
    const auto &s = provisioning.settings();
    const unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 12000) delay(100);
    if (WiFi.status() != WL_CONNECTED) return "WIFI";
    if (!s.tls) return "TLS";

    if (time(nullptr) < 1700000000) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        const unsigned long clockStart = millis();
        while (time(nullptr) < 1700000000 && millis() - clockStart < 8000) delay(100);
        if (time(nullptr) < 1700000000) return "CLOCK";
    }

    WiFiClient reachable;
    reachable.setTimeout(5000);
    if (!reachable.connect(s.backendHost, s.backendPort, 3000)) return "BACKEND";
    reachable.stop();

    WiFiClientSecure secure;
    secure.setCACert(s.caCert);
    secure.setTimeout(10000);
    if (!secure.connect(s.backendHost, s.backendPort, 10000)) return "TLS";

    // Both fields were validated as fixed-format UUID/hex before NVS activation.
    String body = String("{\"deviceId\":\"") + s.deviceId + "\",\"secret\":\"" + s.secret + "\"}";
    secure.print("POST /api/device/verify-setup HTTP/1.1\r\nHost: ");
    secure.print(s.backendHost);
    if (s.backendPort != 443) { secure.print(":"); secure.print(s.backendPort); }
    secure.print("\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    secure.print(body.length());
    secure.print("\r\n\r\n");
    if (secure.print(body) != body.length()) { secure.stop(); return "BACKEND"; }
    // The request body and response are never written to serial or display.
    const String status = secure.readStringUntil('\n');
    secure.stop();
    if (!strncmp(status.c_str(), "HTTP/1.1 200 ", 13) || !strncmp(status.c_str(), "HTTP/1.0 200 ", 13)) return "READY";
    if (!strncmp(status.c_str(), "HTTP/1.1 401 ", 13) || !strncmp(status.c_str(), "HTTP/1.0 401 ", 13)) return "CREDENTIAL";
    if (!strncmp(status.c_str(), "HTTP/1.1 429 ", 13) || !strncmp(status.c_str(), "HTTP/1.0 429 ", 13)) return "RATE_LIMIT";
    return "BACKEND";
}

bool handleSetupCommand(const char *command)
{
    if (strncmp(command, "AHCFG/", 6)) return false;
    if (!strcmp(command, "AHCFG/1 STATUS")) {
        if (!provisioning.configured()) Serial.println("AHCFG/1 STATUS UNCONFIGURED");
        else {
            Serial.print("AHCFG/1 STATUS CONFIGURED ");
            Serial.print(provisioning.settings().deviceId);
            Serial.print(" "); Serial.print(provisioning.settings().deviceNumber);
            Serial.print(" "); Serial.println(provisioning.credentialVersion());
        }
    } else if (!strcmp(command, "AHCFG/1 VERIFY")) {
        if (!provisioning.configured()) Serial.println("AHCFG/1 VERIFY ERROR UNCONFIGURED");
        else {
            setupVerification = verifySetup();
            if (!strcmp(setupVerification, "READY")) {
                Serial.print("AHCFG/1 VERIFY READY ");
                Serial.print(provisioning.settings().deviceId);
                Serial.print(" "); Serial.print(provisioning.settings().deviceNumber);
                Serial.print(" "); Serial.println(provisioning.credentialVersion());
            } else { Serial.print("AHCFG/1 VERIFY ERROR "); Serial.println(setupVerification); }
        }
    } else if (!strncmp(command, "AHCFG/1 UPDATE ", 15)) {
        const auto result = provisioning.update(command + 15);
        if (result == DeviceProvisioning::Result::Saved) {
            repairAwaitingRestart = true;
            setupVerification = "PENDING";
            Serial.print("AHCFG/1 ACK UPDATED "); Serial.print(provisioning.settings().deviceId);
            Serial.print(" "); Serial.print(provisioning.settings().deviceNumber);
            Serial.print(" "); Serial.println(provisioning.credentialVersion());
            WiFi.disconnect();
            WiFi.begin(provisioning.settings().wifiSsid, provisioning.settings().wifiPassword);
        } else if (result == DeviceProvisioning::Result::InvalidCertificate) Serial.println("AHCFG/1 ERROR CERTIFICATE");
        else if (result == DeviceProvisioning::Result::Invalid) Serial.println("AHCFG/1 ERROR INVALID");
        else Serial.println("AHCFG/1 ERROR STORAGE");
    } else if (!strncmp(command, "AHCFG/1 SET ", 12)) {
        const auto result = provisioning.configure(command + 12);
        if (result == DeviceProvisioning::Result::Saved) {
            Serial.print("AHCFG/1 ACK CONFIGURED ");
            Serial.print(provisioning.settings().deviceId);
            Serial.print(" "); Serial.print(provisioning.settings().deviceNumber);
            Serial.print(" "); Serial.println(provisioning.credentialVersion());
            startProtocol();
        } else if (result == DeviceProvisioning::Result::Locked) Serial.println("AHCFG/1 ERROR LOCKED");
        else if (result == DeviceProvisioning::Result::InvalidCertificate) Serial.println("AHCFG/1 ERROR CERTIFICATE");
        else if (result == DeviceProvisioning::Result::Invalid) Serial.println("AHCFG/1 ERROR INVALID");
        else Serial.println("AHCFG/1 ERROR STORAGE");
    } else if (strncmp(command, "AHCFG/1 ", 8)) Serial.println("AHCFG/1 ERROR VERSION");
    else Serial.println("AHCFG/1 ERROR COMMAND");
    return true;
}

uint64_t reference()
{
    const auto &s = protocol.state();
    return s.highestBid.present ? s.highestBid.ore : (s.startingPrice.present ? s.startingPrice.ore : 0);
}

void printAmount(const DeviceProtocol::OptionalAmount &amount)
{
    if (!amount.present) { tft.print("--"); return; }
    char text[32];
    snprintf(text, sizeof(text), "%" PRIu64 " kr", amount.ore / 100 + (amount.ore % 100 >= 50));
    tft.print(text);
}

bool canEditDraft()
{
    const auto &s = protocol.state();
    return protocol.status() == DeviceProtocol::Status::Ready && s.openForBids &&
        s.auctionLotId.length() && reference() > 0;
}

void updateDraft()
{
    const auto &s = protocol.state();
    if (protocol.status() != DeviceProtocol::Status::Ready) return;
    if (!s.auctionLotId.length()) {
        draft = 0; draftAuction = ""; draftLot = ""; return;
    }
    if (draftAuction != s.auctionId || draftLot != s.auctionLotId) {
        draftAuction = s.auctionId; draftLot = s.auctionLotId;
        draft = s.startingPrice.present ? s.startingPrice.ore : 0;
    }
    if (s.highestBid.present && draft < s.highestBid.ore) draft = s.highestBid.ore;
}

void drawScreen()
{
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(2);
    if (!provisioning.configured()) {
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 10); tft.print("USB setup required");
        tft.setTextSize(1);
        tft.setCursor(10, 45); tft.print("Connect USB serial at 115200 baud");
        tft.setCursor(10, 65); tft.print("Send AHCFG/1 STATUS");
        return;
    }
    if (!protocolStarted) {
        tft.setTextColor(ILI9341_YELLOW);
        tft.setCursor(10, 10); tft.print("Bid storage fault");
        tft.setTextSize(1); tft.setCursor(10, 40);
        tft.print("Device locked - technical service required");
        return;
    }
    bool ready = protocol.status() == DeviceProtocol::Status::Ready;
    tft.setTextColor(ready ? ILI9341_GREEN : ILI9341_YELLOW);
    tft.setCursor(10, 10); tft.print(ready ? "Synchronized" : "Not synchronized");
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 28); tft.print("Device "); tft.print(provisioning.settings().deviceNumber);
    tft.setCursor(150, 28); tft.print("Check: "); tft.print(setupVerification);
    if (ready) {
        const auto &s = protocol.state();
        tft.setCursor(10, 40); tft.print("Lot: ");
        tft.print(s.auctionLotId.length() ? s.auctionLotId.c_str() : "--");
        tft.setCursor(10, 65); tft.print("Highest: "); printAmount(s.highestBid);
        tft.setCursor(10, 90); tft.print("Your accepted bid: "); printAmount(s.customerBid);
        tft.setCursor(10, 115); tft.print("Draft: ");
        DeviceProtocol::OptionalAmount displayed;
        displayed.present = s.auctionLotId.length() && (s.startingPrice.present || s.highestBid.present);
        displayed.ore = draft; printAmount(displayed);
    } else {
        tft.setCursor(10, 40); tft.print("Awaiting authoritative state");
    }
    tft.setCursor(10, 150);
    const auto outcome = protocol.receipt().outcome;
    if (outcome == DeviceProtocol::Outcome::Accepted) tft.print("Accepted - press encoder to confirm");
    else if (outcome == DeviceProtocol::Outcome::Rejected) tft.print("Rejected - press encoder to confirm");
    else if (outcome == DeviceProtocol::Outcome::Unknown) tft.print("Unknown - bid button retries/checks");
    else if (protocol.hasPending()) tft.print("Bid pending - await receipt");
    else if (!ready) tft.print("Bidding unavailable until synchronized");
    else if (!canEditDraft()) tft.print("No open lot with positive reference");
    else if (draft > INT64_MAX) tft.print("Draft exceeds protocol bid limit");
    else if (!draft) tft.print("Zero draft - rotate before bidding");
    else tft.print("Encoder adjusts; bid button sends");
    if (protocol.hasPending()) {
        tft.setCursor(10, 214); tft.print("Device ID: "); tft.print(provisioning.settings().deviceId);
        tft.setCursor(10, 168); tft.print("Request: "); tft.print(protocol.receipt().requestId);
        tft.setCursor(10, 182); tft.print("Amount: ");
        DeviceProtocol::OptionalAmount amount{true, protocol.pendingAmountOre()}; printAmount(amount);
        if (outcome == DeviceProtocol::Outcome::Unknown) {
            tft.setCursor(10, 198); tft.print("Different customer: ask staff to check bid");
        } else if (outcome == DeviceProtocol::Outcome::Accepted) {
            tft.setCursor(10, 198); tft.print("Bid ID: "); tft.print(protocol.receipt().bidId);
        } else if (outcome == DeviceProtocol::Outcome::Rejected) {
            tft.setCursor(10, 198); tft.print("Reason: "); tft.print(protocol.receipt().errorCode);
        }
    }
    if (protocol.faultCode()[0]) {
        tft.setCursor(10, 230); tft.print("Fault: "); tft.print(protocol.faultCode());
        tft.print(" free="); tft.print(protocol.faultFreeEntries());
    }
}

void readEncoder()
{
    static const int8_t table[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    int current = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
    transitions += table[(encoderState << 2) | current]; encoderState = current;
    if (transitions >= 4 || transitions <= -4) {
        uint64_t ref = reference();
        if (canEditDraft() && !protocol.hasPending()) {
            uint64_t step = (ref / 10000 + ((ref % 10000) >= 5000)) * 100;
            if (step < 100) step = 100;
            if (transitions >= 4) draft = draft > ref && draft - ref > step ? draft - step : ref;
            else if (UINT64_MAX - draft >= step) {
                draft += step;
                if (draft < ref) draft = ref;
            }
        }
        transitions = 0; drawScreen();
    }
}

void readButton()
{
    bool reading = digitalRead(ENCODER_SW);
    if (reading != lastReading) buttonChanged = millis();
    if (millis() - buttonChanged >= 40 && reading != buttonState) {
        buttonState = reading;
        if (buttonState == LOW && protocol.hasPending() &&
            (protocol.receipt().outcome == DeviceProtocol::Outcome::Accepted ||
             protocol.receipt().outcome == DeviceProtocol::Outcome::Rejected)) {
            if (!protocol.clearPending()) Serial.println("Bid receipt could not be saved as acknowledged.");
            drawScreen();
        } else if (buttonState == LOW && canEditDraft() && !protocol.hasPending()) {
            uint64_t ref = reference();
            uint64_t kr = ref / 1000, rem = ref % 1000;
            if (ref && kr <= (UINT64_MAX / 100 - (rem * 11 + 500) / 1000) / 11) {
                uint64_t amount = (kr * 11 + (rem * 11 + 500) / 1000) * 100;
                const auto &highest = protocol.state().highestBid;
                if (highest.present && amount <= highest.ore) {
                    uint64_t minimumKr = highest.ore / 100 + 1 + (highest.ore % 100 != 0);
                    if (minimumKr > UINT64_MAX / 100) { lastReading = reading; return; }
                    amount = minimumKr * 100;
                }
                draft = amount;
                drawScreen();
            }
        }
    }
    lastReading = reading;
}

void readBidButton()
{
    bool reading = digitalRead(BID_BUTTON);
    if (reading != lastBidReading) bidButtonChanged = millis();
    if (millis() - bidButtonChanged >= 40 && reading != bidButtonState) {
        bidButtonState = reading;
        if (bidButtonState == LOW) {
            if (protocol.hasPending() && protocol.receipt().outcome == DeviceProtocol::Outcome::Unknown) {
                if (!protocol.retryPending() && !protocol.checkPendingResolution())
                    Serial.println("Retry unavailable; original bid remains unresolved.");
            } else if (!canEditDraft() || !draft || draft > INT64_MAX || protocol.hasPending())
                Serial.println("Bid blocked: synchronize, select an open lot and resolve pending bids.");
            else if (!protocol.placeBid(draft))
                Serial.println(protocol.hasPending() ? "Send failed; outcome unknown, reconcile or RETRY." : "Bid not created.");
        }
    }
    lastBidReading = reading;
}

void reportReceipt()
{
    const auto &r = protocol.receipt();
    if (r.outcome == lastOutcome && r.requestId == lastRequest) return;
    lastOutcome = r.outcome; lastRequest = r.requestId;
    if (r.outcome == DeviceProtocol::Outcome::None) return;
    Serial.print("Request "); Serial.print(r.requestId); Serial.print(": ");
    switch (r.outcome) {
    case DeviceProtocol::Outcome::Pending: Serial.println("pending"); break;
    case DeviceProtocol::Outcome::Unknown: Serial.println("UNKNOWN - reconcile or RETRY"); break;
    case DeviceProtocol::Outcome::Accepted:
        Serial.print("accepted; bidId="); Serial.print(r.bidId);
        Serial.print(" duplicate="); Serial.println(r.duplicate ? "yes" : "no"); break;
    case DeviceProtocol::Outcome::Rejected:
        Serial.print("rejected; error="); Serial.println(r.errorCode); break;
    default: break;
    }
    drawScreen();
}

void handleCommand(const char *command)
{
    if (handleSetupCommand(command)) { drawScreen(); return; }
    if (!provisioning.configured()) {
        Serial.println("Awaiting USB setup. Send AHCFG/1 STATUS.");
        return;
    }
    if (*command) Serial.println("USB is reserved for setup and technical service.");
    reportReceipt(); drawScreen();
}

void readSerial()
{
    if ((serialLength || serialOverflow) && millis() - serialChanged >= 5000) {
        serialLength = 0; serialOverflow = false;
        Serial.println("AHCFG/1 ERROR TIMEOUT");
    }
    while (Serial.available()) {
        char c = Serial.read();
        serialChanged = millis();
        if (c == '\n' || c == '\r') {
            if (serialOverflow) Serial.println("AHCFG/1 ERROR TOO_LONG");
            else if (serialLength) { serialCommand[serialLength] = '\0'; handleCommand(serialCommand); }
            serialLength = 0; serialOverflow = false;
        } else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
            serialOverflow = true;
        } else if (!serialOverflow) {
            if (serialLength < sizeof(serialCommand) - 1) serialCommand[serialLength++] = c;
            else serialOverflow = true;
        }
    }
}

void setup()
{
    // A full AHCFG/1 SET frame can arrive while the display is being redrawn.
    // Reserve enough RX space before starting Serial to avoid truncating it.
    Serial.setRxBufferSize(sizeof(serialCommand));
    Serial.begin(115200);
    pinMode(ENCODER_A, INPUT_PULLUP); pinMode(ENCODER_B, INPUT_PULLUP); pinMode(ENCODER_SW, INPUT_PULLUP);
    pinMode(BID_BUTTON, INPUT_PULLUP);
    encoderState = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
    lastReading = buttonState = digitalRead(ENCODER_SW);
    lastBidReading = bidButtonState = digitalRead(BID_BUTTON);
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
    tft.begin(); tft.setRotation(1);
    if (!provisioning.load()) Serial.println("NVS unavailable; awaiting USB setup.");
    if (provisioning.configured()) {
        startProtocol();
        Serial.println("Bidding uses the screen, encoder and bid button. USB is for setup and service.");
    } else Serial.println("Awaiting USB setup. Send AHCFG/1 STATUS.");
    drawScreen();
}

void loop()
{
    readSerial();
    if (provisioning.configured() && !repairAwaitingRestart) {
        protocol.loop(); updateDraft(); reportReceipt(); readEncoder(); readButton(); readBidButton();
    }
    if (millis() - lastDraw >= 1000) { lastDraw = millis(); drawScreen(); }
    delay(1);
}
