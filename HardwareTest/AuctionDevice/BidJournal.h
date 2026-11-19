#pragma once

#include <stdint.h>

// Two alternating NVS records. A torn write leaves the previous complete
// request (or clear marker) available after power loss.
class BidJournal {
public:
    struct Entry {
        char deviceId[37];
        char requestId[37];
        char sessionId[37];
        char auctionId[37];
        char lotId[37];
        char payload[1024];
        uint64_t sessionRevision;
        uint64_t amountOre;
        uint8_t outcome; // 1 pending/unknown, 2 accepted, 3 rejected
        char errorCode[40];
        char bidId[37];
    };

    bool load(const char* deviceId);
    bool save(const Entry& entry);
    bool clear();
    bool hasPending() const { return active_; }
    const Entry& entry() const { return entry_; }
    const char* lastWriteFailure() const { return lastWriteFailure_; }
    uint32_t lastFreeEntries() const { return lastFreeEntries_; }
private:
    Entry entry_{};
    bool active_ = false, ready_ = false;
    uint32_t generation_ = 0;
    uint8_t slot_ = 1;
    char deviceId_[37]{};
    const char* lastWriteFailure_ = "";
    uint32_t lastFreeEntries_ = 0;
    bool write(bool active, const Entry& entry);
};
