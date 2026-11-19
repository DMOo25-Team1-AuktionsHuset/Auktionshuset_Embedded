#include "BidJournal.h"
#include <Preferences.h>
#include <stddef.h>
#include <string.h>
#ifdef ARDUINO_ARCH_ESP32
#include <nvs_flash.h>
#endif

namespace {
constexpr uint32_t Magic = 0x41484231; // AHB1
struct Record {
    uint32_t magic, generation;
    uint8_t active;
    BidJournal::Entry entry;
    uint32_t checksum;
};
Record first, second;
uint32_t crc(const Record& r) {
    uint32_t value = 0xffffffffu;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&r);
    for (size_t i = 0; i < offsetof(Record, checksum); ++i) {
        value ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
    }
    return ~value;
}
bool valid(const Record& r) {
    return r.magic == Magic && r.checksum == crc(r) && r.active <= 1 &&
        r.entry.deviceId[36] == 0 && r.entry.requestId[36] == 0 &&
        r.entry.sessionId[36] == 0 && r.entry.auctionId[36] == 0 &&
        r.entry.lotId[36] == 0 && r.entry.payload[sizeof(r.entry.payload) - 1] == 0;
}
}

bool BidJournal::load(const char* deviceId) {
    ready_ = false; active_ = false; entry_ = {}; generation_ = 0; slot_ = 1;
    if (!deviceId || strlen(deviceId) != 36) return false;
    memcpy(deviceId_, deviceId, 37);
    Preferences p;
    // The namespace does not exist on a device that has never placed a bid.
    // Preferences cannot open a missing namespace read-only; creating an empty
    // namespace here does not create or clear a bid record.
    if (!p.begin("auctionbid", false)) return false;
    const size_t firstLength = p.getBytesLength("bid0"), secondLength = p.getBytesLength("bid1");
    const bool a = firstLength == sizeof(first) &&
        p.getBytes("bid0", &first, sizeof(first)) == sizeof(first) && valid(first);
    const bool b = secondLength == sizeof(second) &&
        p.getBytes("bid1", &second, sizeof(second)) == sizeof(second) && valid(second);
    p.end();
    if (!a && !b) {
        // Empty storage is safe. Corrupt records are not.
        if (firstLength || secondLength) return false;
        ready_ = true; return true;
    }
    const bool useSecond = b && (!a || static_cast<int32_t>(second.generation - first.generation) > 0);
    const Record& selected = useSecond ? second : first;
    // A valid clear marker cannot prove that a damaged other slot did not
    // contain a newer request that was already sent.
    if (!selected.active && ((useSecond && firstLength && !a) ||
                             (!useSecond && secondLength && !b))) return false;
    if (strcmp(selected.entry.deviceId, deviceId_)) return false;
    slot_ = useSecond ? 1 : 0;
    generation_ = selected.generation;
    active_ = selected.active != 0;
    entry_ = selected.entry;
    ready_ = true;
    return true;
}

bool BidJournal::write(bool active, const Entry& entry) {
    // Preserve the first physical storage error across reconnect attempts.
    if (!ready_) return false;
    lastWriteFailure_ = ""; lastFreeEntries_ = 0;
    if (strcmp(entry.deviceId, deviceId_)) { lastWriteFailure_ = "JOURNAL_STATE"; return false; }
    // Reuse the load buffers: the ESP32 loop task stack is too small for two
    // full records plus the protocol's pending message frame.
    Record& next = first;
    next = {};
    next.magic = Magic; next.generation = generation_ + 1;
    next.active = active ? 1 : 0; next.entry = entry; next.checksum = crc(next);
    const char* target = slot_ ? "bid0" : "bid1";
    Preferences p;
    if (!p.begin("auctionbid", false)) { lastWriteFailure_ = "JOURNAL_OPEN"; return false; }
    // The other slot is never the latest complete request. Erase it first so
    // NVS can reclaim its blob entries before allocating the replacement.
    // An interrupted erase still leaves the current slot intact.
    if (p.getBytesLength(target) && !p.remove(target)) {
        p.end(); lastWriteFailure_ = "JOURNAL_ERASE"; ready_ = false; return false;
    }
    const bool written = p.putBytes(target, &next, sizeof(next)) == sizeof(next);
    Record& check = second;
    check = {};
    const bool verified = written && p.getBytesLength(target) == sizeof(check) &&
        p.getBytes(target, &check, sizeof(check)) == sizeof(check) && valid(check) &&
        check.generation == next.generation && check.active == next.active;
    p.end();
    if (!verified) {
        lastWriteFailure_ = written ? "JOURNAL_VERIFY" : "JOURNAL_PUT";
#ifdef ARDUINO_ARCH_ESP32
        nvs_stats_t stats{};
        if (nvs_get_stats(nullptr, &stats) == ESP_OK) lastFreeEntries_ = stats.free_entries;
#endif
        ready_ = false; return false;
    }
    generation_ = next.generation; slot_ = slot_ ? 0 : 1;
    entry_ = entry; active_ = active;
    return true;
}
bool BidJournal::save(const Entry& entry) { return write(true, entry); }
bool BidJournal::clear() {
    if (!active_) return false;
    Entry empty{};
    memcpy(empty.deviceId, deviceId_, 37);
    return write(false, empty);
}
