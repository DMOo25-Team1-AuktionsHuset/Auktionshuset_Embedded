#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <inttypes.h>
#include "config.h"
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
uint64_t draft = 0;
String draftAuction, draftLot;
int encoderState, transitions = 0;
bool lastReading, buttonState, lastBidReading, bidButtonState, biddingEnabled = false;
unsigned long buttonChanged = 0, bidButtonChanged = 0, lastDraw = 0;
DeviceProtocol::Outcome lastOutcome = DeviceProtocol::Outcome::None;
String lastRequest;
char serialCommand[32];
size_t serialLength = 0;
bool serialOverflow = false;

uint64_t reference()
{
    const auto &s = protocol.state();
    return s.highestBid.present ? s.highestBid.ore : (s.startingPrice.present ? s.startingPrice.ore : 0);
}

void printAmount(const DeviceProtocol::OptionalAmount &amount)
{
    if (!amount.present) { tft.print("--"); return; }
    char text[32];
    snprintf(text, sizeof(text), "%" PRIu64 ".%02u kr", amount.ore / 100, (unsigned)(amount.ore % 100));
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
    bool ready = protocol.status() == DeviceProtocol::Status::Ready;
    tft.setTextColor(ready ? ILI9341_GREEN : ILI9341_YELLOW);
    tft.setCursor(10, 10); tft.print(ready ? "Synchronized" : "Not synchronized");
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
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
    if (outcome == DeviceProtocol::Outcome::Accepted) tft.print("Accepted - record receipt, then ACK");
    else if (outcome == DeviceProtocol::Outcome::Rejected) tft.print("Rejected - record receipt, then ACK");
    else if (outcome == DeviceProtocol::Outcome::Unknown) tft.print("Unknown - RETRY or reconcile");
    else if (protocol.hasPending()) tft.print("Bid pending - await receipt");
    else if (!biddingEnabled) tft.print("Reconcile on Serial before bidding");
    else if (!ready) tft.print("Bidding unavailable until synchronized");
    else if (!canEditDraft()) tft.print("No open lot with positive reference");
    else if (draft > INT64_MAX) tft.print("Draft exceeds protocol bid limit");
    else if (!draft) tft.print("Zero draft - rotate before bidding");
    else tft.print("Encoder adjusts; bid button sends");
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
        if (buttonState == LOW && canEditDraft() && !protocol.hasPending()) {
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
            if (!biddingEnabled || !canEditDraft() || !draft || draft > INT64_MAX || protocol.hasPending())
                Serial.println("Bid blocked: reconcile, synchronize, select an open lot and resolve pending bids.");
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
    if (!strcmp(command, "RECONCILED")) {
        if (protocol.hasPending() && protocol.receipt().outcome != DeviceProtocol::Outcome::Unknown) {
            Serial.println("Only an unknown bid can be cleared by reconciliation; use ACK for a final receipt.");
            return;
        }
        if (protocol.hasPending()) protocol.clearPending();
        protocol.enableBiddingAfterReconciliation(); biddingEnabled = true;
        Serial.println("Reconciliation confirmed; bidding enabled when synchronized.");
    } else if (!strcmp(command, "ACK")) {
        auto outcome = protocol.receipt().outcome;
        if (protocol.hasPending() && (outcome == DeviceProtocol::Outcome::Accepted || outcome == DeviceProtocol::Outcome::Rejected)) {
            protocol.clearPending(); Serial.println("Final receipt recorded; pending cleared.");
        } else Serial.println("No final receipt to acknowledge.");
    } else if (!strcmp(command, "RETRY")) {
        if ((protocol.receipt().outcome != DeviceProtocol::Outcome::Unknown &&
                     protocol.receipt().outcome != DeviceProtocol::Outcome::Pending) || !protocol.retryPending())
            Serial.println("Retry not sent; original bid remains unresolved.");
        else Serial.println("Original bid retransmitted; outcome remains unknown until receipt.");
    } else if (*command) Serial.println("Commands: RECONCILED, RETRY, ACK; use bid button to bid");
    reportReceipt(); drawScreen();
}

void readSerial()
{
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialOverflow) Serial.println("Command too long.");
            else if (serialLength) { serialCommand[serialLength] = '\0'; handleCommand(serialCommand); }
            serialLength = 0; serialOverflow = false;
        } else if (!serialOverflow) {
            if (serialLength < sizeof(serialCommand) - 1) serialCommand[serialLength++] = c;
            else serialOverflow = true;
        }
    }
}

void setup()
{
    Serial.begin(115200);
    pinMode(ENCODER_A, INPUT_PULLUP); pinMode(ENCODER_B, INPUT_PULLUP); pinMode(ENCODER_SW, INPUT_PULLUP);
    pinMode(BID_BUTTON, INPUT_PULLUP);
    encoderState = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
    lastReading = buttonState = digitalRead(ENCODER_SW);
    lastBidReading = bidButtonState = digitalRead(BID_BUTTON);
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
    tft.begin(); tft.setRotation(1);
    const DeviceProtocol::Config config = { DEVICE_WIFI_SSID, DEVICE_WIFI_PASSWORD, DEVICE_BACKEND_HOST,
        DEVICE_BACKEND_PORT, DEVICE_BACKEND_PATH, DEVICE_ID, DEVICE_CREDENTIAL, DEVICE_TLS, DEVICE_TLS_CA_CERT };
    if (!protocol.begin(config)) Serial.println("Invalid device configuration");
    Serial.println("Reconcile any pre-restart bid, then send RECONCILED. Commands: RETRY, ACK. Use the bid button to bid.");
    drawScreen();
}

void loop()
{
    protocol.loop(); updateDraft(); reportReceipt(); readEncoder(); readButton(); readBidButton(); readSerial();
    if (millis() - lastDraw >= 1000) { lastDraw = millis(); drawScreen(); }
    delay(1);
}
