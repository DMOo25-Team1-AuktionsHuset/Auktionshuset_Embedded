#include "DeviceProtocol.h"
#include <ArduinoJson.h>
#include <esp_system.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

namespace {
constexpr size_t MaxMessage = 4096;
constexpr uint64_t MaxProtocolNumber = INT64_MAX;
bool uuidValid(const char* s) {
    if (!s || strlen(s) != 36) return false;
    bool nonzero = false;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else { if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false; if (s[i] != '0') nonzero = true; }
    }
    return nonzero;
}
String newUuid() {
    uint8_t b[16]; esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 15) | 0x40; b[8] = (b[8] & 63) | 0x80;
    char s[37];
    snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return String(s);
}
String decimal(uint64_t n) {
    char buf[21]; char* p = buf + 20; *p = 0;
    do { *--p = '0' + n % 10; n /= 10; } while (n);
    return String(p);
}
bool number(JsonVariantConst v, uint64_t& out, uint64_t max = MaxProtocolNumber) {
    if (!v.is<const char*>()) return false;
    const char* p = v.as<const char*>(); if (!p || !*p || strlen(p) > 20) return false;
    uint64_t n = 0;
    for (; *p; ++p) { if (*p < '0' || *p > '9') return false; unsigned d = *p - '0'; if (n > (max - d) / 10) return false; n = n * 10 + d; }
    out = n; return true;
}
bool optionalNumber(JsonVariantConst v, DeviceProtocol::OptionalAmount& out, uint64_t max = MaxProtocolNumber) {
    if (v.isNull()) { out = {}; return true; }
    out.present = true; return number(v, out.ore, max);
}
bool id(JsonVariantConst v, String& out, bool nullable = false) {
    if (nullable && v.isNull()) { out = ""; return true; }
    if (!v.is<const char*>() || !uuidValid(v.as<const char*>())) return false;
    out = v.as<const char*>(); return true;
}
bool same(JsonVariantConst v, const String& s) { return v.is<const char*>() && s == v.as<const char*>(); }
}

bool DeviceProtocol::begin(const Config& c) {
    if (started_ || !c.ssid || !c.password || !c.host || !*c.host || !c.path || !*c.path ||
        !uuidValid(c.deviceId) || !c.credential || strlen(c.credential) != 64 ||
        (c.tls && (!c.caCert || !*c.caCert))) return false;
    config_ = c; started_ = true; WiFi.mode(WIFI_STA); WiFi.begin(c.ssid, c.password); lastWifiAttempt_ = millis();
    socket_.onEvent([this](WStype_t t, uint8_t* p, size_t n) { event(t, p, n); });
    socket_.setReconnectInterval(5000);
    return true;
}
void DeviceProtocol::loop() {
    if (!started_) return;
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (socketStarted_) { socket_.disconnect(); socketStarted_ = false; offline(); }
        if (now - lastWifiAttempt_ >= 10000) { lastWifiAttempt_ = now; WiFi.disconnect(); WiFi.begin(config_.ssid, config_.password); }
        return;
    }
    if (!socketStarted_) {
        socketStarted_ = true;
        if (config_.tls) socket_.beginSslWithCA(config_.host, config_.port, config_.path, config_.caCert, "");
        else socket_.begin(config_.host, config_.port, config_.path, "");
    }
    socket_.loop();
    now = millis();
    if (fragmenting_ && now - fragmentStarted_ >= 15000) { fail(); return; }
    if (status_ != Status::Offline && now - lastReceived_ >= 45000) { socket_.disconnect(); offline(); return; }
    if (authenticated_ && now - lastPing_ >= 15000) { lastPing_ = now; if (!sendHeartbeat("ping")) { socket_.disconnect(); offline(); } }
}
void DeviceProtocol::offline() {
    authenticated_ = false; haveState_ = false; fragmenting_ = false; fragments_ = ""; status_ = Status::Offline;
    if (hasPending() && (receipt_.outcome == Outcome::Pending || receipt_.outcome == Outcome::Unknown)) { uncertain_ = true; receipt_.outcome = Outcome::Unknown; }
}
void DeviceProtocol::fail() { socket_.disconnect(); offline(); status_ = Status::Fault; }
void DeviceProtocol::event(WStype_t type, uint8_t* p, size_t n) {
    switch (type) {
    case WStype_CONNECTED: {
        offline(); status_ = Status::Authenticating; lastReceived_ = lastPing_ = millis(); authRequest_ = newUuid();
        JsonDocument doc; doc["protocolVersion"] = 1; doc["type"] = "authenticate"; doc["requestId"] = authRequest_;
        doc["deviceId"] = config_.deviceId; doc["credential"] = config_.credential;
        String text; serializeJson(doc, text); if (!socket_.sendTXT(text)) fail(); break;
    }
    case WStype_DISCONNECTED: offline(); break;
    case WStype_TEXT:
        if (fragmenting_ || n > MaxMessage) { fail(); break; }
        message(p, n); break;
    case WStype_FRAGMENT_TEXT_START:
        if (fragmenting_ || n > MaxMessage) { fail(); break; }
        fragmenting_ = true; fragmentStarted_ = millis(); fragments_ = "";
        if (n && (!p || !fragments_.concat(reinterpret_cast<const char*>(p), n))) fail();
        break;
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
        if (!fragmenting_ || n > MaxMessage - fragments_.length() || (n && (!p || !fragments_.concat(reinterpret_cast<const char*>(p), n)))) { fail(); break; }
        if (type == WStype_FRAGMENT_FIN) { fragmenting_ = false; message(reinterpret_cast<const uint8_t*>(fragments_.c_str()), fragments_.length()); fragments_ = ""; }
        break;
    case WStype_BIN: case WStype_FRAGMENT_BIN_START: fail(); break;
    default: break;
    }
}
bool DeviceProtocol::sendHeartbeat(const char* type) {
    JsonDocument doc; doc["protocolVersion"] = 1; doc["type"] = type; doc["deviceId"] = config_.deviceId; doc["sessionId"] = sessionId_;
    String text; serializeJson(doc, text); return socket_.sendTXT(text);
}
void DeviceProtocol::message(const uint8_t* payload, size_t length) {
    if (!payload || !length || memchr(payload, 0, length)) { fail(); return; }
    JsonDocument doc;
    if (deserializeJson(doc, payload, length) || !doc.is<JsonObject>() || !doc["protocolVersion"].is<int>() || doc["protocolVersion"].as<int>() != 1 || !doc["type"].is<const char*>()) { fail(); return; }
    const char* type = doc["type"];
    if (!strcmp(type, "authResult")) {
        if (status_ != Status::Authenticating || !same(doc["requestId"], authRequest_) || !same(doc["deviceId"], String(config_.deviceId)) || !doc["accepted"].is<bool>()) { fail(); return; }
        if (!doc["accepted"].as<bool>()) { fail(); return; }
        String session; uint64_t revision;
        if (!id(doc["sessionId"], session) || !number(doc["sessionRevision"], revision)) { fail(); return; }
        if (sessionId_ != session || sessionRevision_ != revision) { revisionKnown_ = false; minimumRevision_ = 0; }
        sessionId_ = session; sessionRevision_ = revision; authenticated_ = true; haveState_ = false; status_ = Status::Synchronizing;
    } else {
        if (!authenticated_ || !same(doc["deviceId"], String(config_.deviceId)) || !same(doc["sessionId"], sessionId_)) { fail(); return; }
        if (!strcmp(type, "ping") || !strcmp(type, "pong")) {
            if (!strcmp(type, "ping") && !sendHeartbeat("pong")) { fail(); return; }
        } else if (!strcmp(type, "state")) {
            State next; uint64_t rev;
            if (!number(doc["sessionRevision"], rev) || rev != sessionRevision_ ||
                !id(doc["customerId"], next.customerId) || !id(doc["auctionId"], next.auctionId) ||
                !id(doc["auctionLotId"], next.auctionLotId, true) || !number(doc["auctionRevision"], next.auctionRevision) ||
                (!doc["openForBids"].isNull() && !doc["openForBids"].is<bool>()) || !optionalNumber(doc["startingPriceOre"], next.startingPrice, UINT64_MAX) ||
                !optionalNumber(doc["highestBidOre"], next.highestBid, UINT64_MAX) || !optionalNumber(doc["customerBidOre"], next.customerBid, UINT64_MAX) ||
                !optionalNumber(doc["bidSequence"], next.bidSequence)) { fail(); return; }
            if (revisionKnown_ && (next.auctionId != state_.auctionId || next.auctionRevision < state_.auctionRevision)) { fail(); return; }
            next.sessionRevision = rev; next.openForBids = doc["openForBids"].as<bool>(); state_ = next; haveState_ = true; revisionKnown_ = true; status_ = Status::Synchronizing;
        } else if (!strcmp(type, "connectionStatus")) {
            uint64_t rev; if (!number(doc["sessionRevision"], rev) || rev != sessionRevision_ || !doc["status"].is<const char*>()) { fail(); return; }
            const char* s = doc["status"];
            if (!strcmp(s, "synchronized") && haveState_) status_ = state_.auctionRevision >= minimumRevision_ ? Status::Ready : Status::Synchronizing;
            else if (!strcmp(s, "synchronizing")) status_ = Status::Synchronizing;
            else if (!strcmp(s, "unavailable")) { status_ = Status::Unavailable; haveState_ = false; }
            else if (!strcmp(s, "released")) { status_ = Status::Released; haveState_ = false; if (hasPending() && (receipt_.outcome == Outcome::Pending || receipt_.outcome == Outcome::Unknown)) { uncertain_ = true; receipt_.outcome = Outcome::Unknown; } }
            else { fail(); return; }
        } else if (!strcmp(type, "bidReceipt")) {
            uint64_t rev; String auction, lot;
            if (!hasPending() || !same(doc["requestId"], receipt_.requestId) || !number(doc["sessionRevision"], rev) || rev != pendingSessionRevision_ || sessionId_ != pendingSession_ ||
                !id(doc["auctionId"], auction) || auction != pendingAuction_ || !id(doc["auctionLotId"], lot) || lot != pendingLot_ ||
                !doc["accepted"].is<bool>() || !doc["duplicate"].is<bool>()) { fail(); return; }
            if (receipt_.outcome == Outcome::Accepted || receipt_.outcome == Outcome::Rejected) { lastReceived_ = millis(); return; }
            Receipt r; r.requestId = receipt_.requestId; r.duplicate = doc["duplicate"].as<bool>();
            if (!id(doc["bidId"], r.bidId, true) || !optionalNumber(doc["bidSequence"], r.bidSequence) || !optionalNumber(doc["auctionRevision"], r.auctionRevision) ||
                (!doc["errorCode"].isNull() && !doc["errorCode"].is<const char*>())) { fail(); return; }
            if (!doc["errorCode"].isNull()) r.errorCode = doc["errorCode"].as<const char*>();
            bool accepted = doc["accepted"].as<bool>();
            if (accepted && (r.errorCode.length() || !r.bidId.length() || !r.auctionRevision.present)) { fail(); return; }
            if (!accepted && !r.errorCode.length()) { fail(); return; }
            r.outcome = accepted ? Outcome::Accepted : ((r.errorCode == "Unavailable" || (uncertain_ && !r.duplicate)) ? Outcome::Unknown : Outcome::Rejected);
            if (r.outcome == Outcome::Unknown) uncertain_ = true;
            if (r.auctionRevision.present && r.auctionRevision.ore > minimumRevision_) minimumRevision_ = r.auctionRevision.ore;
            if (haveState_ && state_.auctionRevision < minimumRevision_) status_ = Status::Synchronizing;
            receipt_ = r;
        } else { fail(); return; }
    }
    lastReceived_ = millis();
}
bool DeviceProtocol::placeBid(uint64_t amountOre) {
    if (!biddingEnabled_ || hasPending() || status_ != Status::Ready || !haveState_ || !state_.openForBids || !state_.auctionLotId.length() || amountOre > MaxProtocolNumber) return false;
    String request = newUuid();
    JsonDocument doc; doc["protocolVersion"] = 1; doc["type"] = "placeBid"; doc["requestId"] = request; doc["deviceId"] = config_.deviceId;
    doc["sessionId"] = sessionId_; doc["sessionRevision"] = decimal(sessionRevision_); doc["auctionId"] = state_.auctionId;
    doc["auctionLotId"] = state_.auctionLotId; doc["auctionRevision"] = decimal(state_.auctionRevision); doc["amountOre"] = decimal(amountOre);
    String text; if (!serializeJson(doc, text) || text.length() > MaxMessage) return false;
    pending_ = text; pendingSession_ = sessionId_; pendingSessionRevision_ = sessionRevision_; pendingAuction_ = state_.auctionId; pendingLot_ = state_.auctionLotId;
    uncertain_ = false; receipt_ = {}; receipt_.requestId = request; receipt_.outcome = Outcome::Pending;
    if (!socket_.sendTXT(pending_)) { uncertain_ = true; receipt_.outcome = Outcome::Unknown; socket_.disconnect(); offline(); return false; }
    return true;
}
bool DeviceProtocol::retryPending() {
    if (!hasPending() || (receipt_.outcome != Outcome::Unknown && receipt_.outcome != Outcome::Pending) || status_ != Status::Ready ||
        sessionId_ != pendingSession_ || sessionRevision_ != pendingSessionRevision_) return false;
    uncertain_ = true; receipt_.outcome = Outcome::Unknown;
    if (!socket_.sendTXT(pending_)) { socket_.disconnect(); offline(); return false; }
    return true;
}
void DeviceProtocol::clearPending() {
    pending_ = ""; pendingSession_ = ""; pendingAuction_ = ""; pendingLot_ = ""; receipt_ = {}; uncertain_ = false;
}
