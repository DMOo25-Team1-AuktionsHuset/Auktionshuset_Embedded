#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> nvsBlob;
inline std::vector<uint8_t> nvsAlternateBlob;
inline std::vector<uint8_t> bidBlob0, bidBlob1;
inline size_t nvsWriteLimit = SIZE_MAX;
inline bool nvsAvailable = true;
inline bool configNamespaceExists = false;
inline bool bidNamespaceExists = false;

struct Preferences {
    static std::vector<uint8_t>& blob(const char* key) {
        if (std::strcmp(key, "bid0") == 0) return bidBlob0;
        if (std::strcmp(key, "bid1") == 0) return bidBlob1;
        return std::strcmp(key, "config2") == 0 ? nvsAlternateBlob : nvsBlob;
    }
    bool begin(const char* name, bool readOnly) {
        if (!nvsAvailable) return false;
        if (std::strcmp(name, "auctiondev") == 0) {
            if (readOnly && !configNamespaceExists) return false;
            if (!readOnly) configNamespaceExists = true;
        } else if (std::strcmp(name, "auctionbid") == 0) {
            if (readOnly && !bidNamespaceExists) return false;
            if (!readOnly) bidNamespaceExists = true;
        }
        return true;
    }
    void end() {}
    size_t getBytesLength(const char* key) { return blob(key).size(); }
    size_t getBytes(const char* key, void* data, size_t length) {
        if (blob(key).size() != length) return 0;
        memcpy(data, blob(key).data(), length);
        return length;
    }
    size_t putBytes(const char* key, const void* data, size_t length) {
        const size_t written = std::min(length, nvsWriteLimit);
        const auto* bytes = static_cast<const uint8_t*>(data);
        blob(key).assign(bytes, bytes + written);
        return written;
    }
    bool remove(const char* key) {
        if (blob(key).empty()) return false;
        blob(key).clear();
        return true;
    }
};
