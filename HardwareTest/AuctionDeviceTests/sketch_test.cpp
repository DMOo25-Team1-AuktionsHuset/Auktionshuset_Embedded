#include <string>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "stubs/Preferences.h"
#define private public
#include "../AuctionDevice/AuctionDevice.ino"
#undef private
unsigned long now = 0;
int pins[40] = {};
SerialType Serial;
WiFiType WiFi;
bool wifiConnected = true, backendReachable = true, tlsValid = true;
int httpStatus = 200;
std::string lastHttpRequest;
SPIType SPI;
int sends = 0, retries = 0;
uint64_t sentAmount = 0;
bool DeviceProtocol::begin(const Config&) { return true; }
void DeviceProtocol::loop() {}
bool DeviceProtocol::placeBid(uint64_t amount) { ++sends; sentAmount = amount; pending_ = "pending"; receipt_.outcome = Outcome::Pending; return true; }
bool DeviceProtocol::retryPending() { ++retries; return true; }
bool DeviceProtocol::requestResolution() { return true; }
bool DeviceProtocol::clearPending() { pending_ = ""; receipt_ = {}; return true; }
int checks = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); } std::printf("PASS: %s\n", name); }
void reset() {
    provisioning.configured_ = true; provisioning.settings_.deviceNumber = 42;
    protocol.status_ = DeviceProtocol::Status::Ready; protocol.state_ = {}; protocol.receipt_ = {}; protocol.pending_ = "";
    protocol.state_.auctionId = "auction"; protocol.state_.auctionLotId = "lot"; protocol.state_.openForBids = true; protocol.state_.startingPrice = {true, 10000};
    draft = 0; draftAuction = draftLot = ""; transitions = 0; encoderState = 0; pins[4] = pins[5] = 0; pins[6] = pins[7] = 1;
    lastReading = buttonState = lastBidReading = bidButtonState = 1; buttonChanged = bidButtonChanged = now = 0; sends = retries = 0; sentAmount = 0; updateDraft();
}
std::string hex(const std::string& value) {
    const char* digits = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : value) { output += digits[c >> 4]; output += digits[c & 15]; }
    return output;
}
void detent(bool increase) {
    int a[] = {1,3,2,0}, b[] = {2,3,1,0}; int* seq = increase ? a : b;
    for (int i=0;i<4;i++) { pins[4] = seq[i] >> 1; pins[5] = seq[i] & 1; readEncoder(); }
}
void pressEncoder() { pins[6]=0; readButton(); now+=40; readButton(); }
void pressBid() { pins[7]=0; readBidButton(); now+=40; readBidButton(); }
void releaseBid() { pins[7]=1; readBidButton(); now+=40; readBidButton(); }
int main() {
    reset(); check(draft == 10000, "new lot uses starting price");
    detent(true); check(draft == 10100 && sends == 0, "encoder edits without sending");
    detent(false); detent(false); check(draft == 10000, "downward clamp at reference");
    reset(); protocol.state_.startingPrice.ore=14999; draft=14999; detent(true); check(draft==15099, "one-percent rounding below midpoint");
    reset(); protocol.state_.startingPrice.ore=15000; draft=15000; detent(true); check(draft==15200, "one-percent rounding at midpoint");
    reset(); protocol.state_.startingPrice.ore=15001; draft=15001; detent(true); check(draft==15201, "one-percent rounding above midpoint");
    reset(); pressEncoder(); check(draft == 11000 && sends == 0, "encoder press adjusts only");
    reset(); protocol.state_.startingPrice.ore=10499; pressEncoder(); check(draft==11500, "110-percent rounding below midpoint");
    reset(); protocol.state_.startingPrice.ore=10500; pressEncoder(); check(draft==11600, "110-percent rounding at midpoint");
    reset(); protocol.state_.startingPrice.ore=10501; pressEncoder(); check(draft==11600, "110-percent rounding above midpoint");
    reset(); protocol.state_.startingPrice.ore=1; draft=1; pressEncoder(); check(draft==0, "1 ore press rounds to zero"); detent(true); check(draft==100, "1 ore encoder restores zero draft");
    reset(); protocol.state_.startingPrice.ore=45; draft=45; pressEncoder(); check(draft==0, "45 ore press rounds to zero"); detent(true); check(draft==100, "45 ore encoder restores zero draft");
    reset(); protocol.state_.startingPrice.ore=46; draft=46; pressEncoder(); check(draft==100, "46 ore press rounds to one krone"); detent(true); check(draft==200, "46 ore encoder increments rounded draft");
    reset(); protocol.state_.highestBid={true,1}; pressEncoder(); check(draft==200, "press ensures one krone above fractional highest");
    reset(); pins[6]=0; readButton(); now=39; readButton(); check(draft==10000, "encoder button debounce before 40ms"); now=40; readButton(); check(draft==11000, "encoder button debounce at 40ms");
    reset(); detent(true); updateDraft(); check(draft==10100, "manual draft retained on refresh"); protocol.state_.highestBid={true,12000}; updateDraft(); check(draft==12000, "higher bid raises draft"); protocol.state_.auctionId="other"; protocol.state_.highestBid={}; updateDraft(); check(draft==10000, "auction change resets draft");
    reset(); protocol.state_.startingPrice.ore=0; updateDraft(); detent(true); pressEncoder(); check(draft==10000 && !canEditDraft(), "zero reference prevents edits");
    reset(); handleCommand("BID"); check(sends==0, "serial BID cannot submit");
    pins[7]=0; readBidButton(); now+=39; readBidButton(); check(sends==0, "bid debounce before 40ms"); now++; readBidButton(); check(sends==1 && sentAmount==10000 && protocol.hasPending(), "GPIO7 sends draft at 40ms"); now+=100; readBidButton(); check(sends==1, "held bid button does not resend");
    detent(true); pressEncoder(); releaseBid(); pressBid(); check(draft==10000 && sends==1, "pending blocks edits and second bid"); handleCommand("ACK"); handleCommand("RECONCILED"); check(protocol.hasPending(), "serial commands cannot clear pending");
    pins[6]=1; readButton(); now+=40; readButton();
    protocol.receipt_.outcome=DeviceProtocol::Outcome::Rejected; protocol.receipt_.errorCode="BidTooLow"; pressEncoder(); check(!protocol.hasPending() && draft==10000, "encoder acknowledges final receipt");
    reset(); detent(true); protocol.status_=DeviceProtocol::Status::Offline; updateDraft(); detent(true); pressBid(); check(draft==10100 && sends==0, "offline retains draft but blocks edits and bids"); protocol.status_=DeviceProtocol::Status::Ready; updateDraft(); check(draft==10100, "same lot reconnect retains manual draft");
    reset(); protocol.state_.openForBids=false; pressBid(); check(sends==0, "closed lot blocks physical bid");
    reset(); protocol.pending_="pending"; protocol.receipt_.outcome=DeviceProtocol::Outcome::Unknown; pressBid(); check(retries==1 && protocol.hasPending(), "unknown physical retry retains pending");
    nvsBlob.clear(); provisioning.load(); Serial.output.clear();
    handleCommand("AHCFG/1 STATUS"); check(Serial.output.find("STATUS UNCONFIGURED") != std::string::npos, "USB status identifies unconfigured device");
    std::string secret(64, 'A'), password = "password1";
    std::string setupLine = "AHCFG/1 SET 11111111-1111-4111-8111-111111111111 42 " + secret +
        " " + hex("AuctionWifi") + " " + hex(password) + " " + hex("backend.test") + " 443 0 -";
    Serial.input = setupLine; Serial.position = 0; readSerial();
    check(!provisioning.configured() && nvsBlob.empty(), "incomplete USB frame cannot activate configuration");
    now += 5000; readSerial(); check(!provisioning.configured() && nvsBlob.empty(), "timed-out USB frame is discarded");
    Serial.input = setupLine; Serial.input.push_back('\0'); Serial.input += "ignored\n";
    Serial.position = 0; readSerial();
    check(!provisioning.configured() && nvsBlob.empty(), "binary bytes in USB frame cannot hide trailing data");
    Serial.input = std::string(8192, 'A') + "\n"; Serial.position = 0; readSerial();
    check(!provisioning.configured() && nvsBlob.empty(), "oversized USB frame cannot activate configuration");
    Serial.input = setupLine + "\n"; Serial.position = 0; readSerial();
    check(provisioning.configured(), "complete USB frame activates configuration");
    handleCommand("AHCFG/1 STATUS");
    check(Serial.output.find("STATUS CONFIGURED 11111111-1111-4111-8111-111111111111 42") != std::string::npos,
          "configured device identifies itself over USB");
    check(Serial.output.find(secret) == std::string::npos && Serial.output.find(password) == std::string::npos,
          "USB status and acknowledgement do not expose secrets");
    handleCommand(setupLine.c_str()); check(Serial.output.find("ERROR LOCKED") != std::string::npos,
          "USB setup cannot overwrite a configured device");
    handleCommand("AHCFG/2 STATUS"); check(Serial.output.find("ERROR VERSION") != std::string::npos,
          "unsupported USB protocol version is rejected");
    provisioning.settings_.tls = 1;
    std::strcpy(provisioning.settings_.caCert, "test CA");
    Serial.output.clear(); lastHttpRequest.clear();
    handleCommand("AHCFG/1 VERIFY");
    check(Serial.output.find("VERIFY READY 11111111-1111-4111-8111-111111111111 42") != std::string::npos,
          "setup verifies Wi-Fi, TLS and backend credential without a customer session");
    check(lastHttpRequest.find("POST /api/device/verify-setup") != std::string::npos &&
          Serial.output.find(secret) == std::string::npos, "setup sends credential only to backend over TLS");
    tlsValid = false; Serial.output.clear(); handleCommand("AHCFG/1 VERIFY");
    check(Serial.output.find("VERIFY ERROR TLS") != std::string::npos, "invalid TLS certificate is reported safely");
    tlsValid = true; httpStatus = 401; Serial.output.clear(); handleCommand("AHCFG/1 VERIFY");
    check(Serial.output.find("VERIFY ERROR CREDENTIAL") != std::string::npos, "wrong or disabled credential is reported safely");
    httpStatus = 200; backendReachable = false; Serial.output.clear(); handleCommand("AHCFG/1 VERIFY");
    check(Serial.output.find("VERIFY ERROR BACKEND") != std::string::npos, "backend failure is reported");
    backendReachable = true; wifiConnected = false; Serial.output.clear(); handleCommand("AHCFG/1 VERIFY");
    check(Serial.output.find("VERIFY ERROR WIFI") != std::string::npos, "Wi-Fi failure is reported");
    std::printf("%d checks passed\n", checks);
}
