#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <stdint.h>
#include "BidJournal.h"

// ESP32; requires ArduinoJson and arduinoWebSockets. Call loop() frequently.
// Configuration strings must remain valid for the lifetime of this object.
class DeviceProtocol {
public:
    struct Config {
        const char* ssid;
        const char* password;
        const char* host;
        uint16_t port;
        const char* path;          // normally /ws/device
        const char* deviceId;      // lowercase canonical UUID
        const char* credential;    // 64 characters; never embed in source control
        bool tls;
        const char* caCert;        // PEM trust anchor; required for TLS, keep alive
    };
    enum class Status { Offline, Authenticating, Synchronizing, Ready, Unavailable, Released, Fault };
    enum class Outcome { None, Pending, Unknown, Accepted, Rejected };
    struct OptionalAmount { bool present = false; uint64_t ore = 0; };
    struct State {
        String customerId, auctionId, auctionLotId;
        uint64_t sessionRevision = 0, auctionRevision = 0;
        bool openForBids = false;
        OptionalAmount startingPrice, highestBid, customerBid, bidSequence;
    };
    struct Receipt {
        Outcome outcome = Outcome::None;
        String requestId, errorCode, bidId;
        bool duplicate = false;
        OptionalAmount bidSequence, auctionRevision;
    };

    bool begin(const Config& config);
    void loop();
    Status status() const { return status_; }
    const State& state() const { return state_; }
    const Receipt& receipt() const { return receipt_; }
    const char* faultCode() const { return faultCode_; }
    uint32_t faultFreeEntries() const { return journal_.lastFreeEntries(); }
    bool hasPending() const { return pending_.length() != 0; }
    uint64_t pendingAmountOre() const { return journal_.entry().amountOre; }
    // Generates a request UUID and retains its complete payload until resolved.
    // Boot loads the durable journal; an unresolved request blocks every new bid.
    bool placeBid(uint64_t amountOre);
    // Resends identical bytes only after authentication and synchronization in the same session.
    bool retryPending();
    bool checkPendingResolution() { return authenticated_ && requestResolution(); }
    // Call only after external reconciliation, or after recording a final receipt.
    bool clearPending();

private:
    Config config_{};
    BidJournal journal_;
    BidJournal::Entry stagedBid_{};
    WebSocketsClient socket_;
    Status status_ = Status::Offline;
    State state_{};
    Receipt receipt_{};
    String sessionId_, authRequest_, pending_, pendingSession_, pendingAuction_, pendingLot_;
    uint64_t sessionRevision_ = 0, pendingSessionRevision_ = 0;
    String fragments_;
    bool biddingEnabled_ = false, uncertain_ = false, revisionKnown_ = false;
    uint64_t minimumRevision_ = 0;
    bool started_ = false, socketStarted_ = false, authenticated_ = false, haveState_ = false, fragmenting_ = false;
    bool clockSyncRequested_ = false;
    const char* faultCode_ = "";
    const char* receivePhase_ = "RX";
    unsigned long lastWifiAttempt_ = 0, lastReceived_ = 0, lastPing_ = 0, fragmentStarted_ = 0;
    void event(WStype_t type, uint8_t* payload, size_t length);
    void message(const uint8_t* payload, size_t length);
    void fail();
    void offline();
    bool sendHeartbeat(const char* type);
    bool requestResolution();
};
