#include "DeviceProvisioning.h"

#include <Preferences.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO_ARCH_ESP32
#include <mbedtls/x509_crt.h>
#endif

namespace {
constexpr uint32_t Magic = 0x41484331; // AHC1
constexpr uint16_t SchemaVersion = 1;
constexpr size_t MaxFields = 8192;
constexpr const char* Namespace = "auctiondev";
constexpr const char* Key = "config";
constexpr const char* AlternateKey = "config2";

struct Record {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    DeviceProvisioning::Settings settings;
    uint32_t checksum;
};

// Static buffers keep the ESP32 loop task stack small. Provisioning is single-threaded.
Record record;
Record alternate;
char input[MaxFields];

size_t boundedLength(const char* value, size_t capacity) {
    size_t length = 0;
    while (length < capacity && value[length]) ++length;
    return length;
}

bool textValid(const char* value, size_t capacity, bool pem = false) {
    const size_t length = boundedLength(value, capacity);
    if (length == capacity) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == 0x7f || (c < 0x20 && !(pem && (c == '\r' || c == '\n')))) return false;
    }
    return true;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decodeHex(const char* token, char* output, size_t capacity) {
    if (!strcmp(token, "-")) { output[0] = 0; return true; }
    const size_t length = strlen(token);
    if (!length || length % 2 || length / 2 >= capacity) return false;
    for (size_t i = 0; i < length / 2; ++i) {
        const int high = hexValue(token[i * 2]);
        const int low = hexValue(token[i * 2 + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
        output[i] = static_cast<char>((high << 4) | low);
    }
    output[length / 2] = 0;
    return true;
}

bool copyToken(const char* token, char* output, size_t capacity) {
    const size_t length = strlen(token);
    if (length >= capacity) return false;
    memcpy(output, token, length + 1);
    return true;
}

bool unsignedNumber(const char* token, uint32_t maximum, uint32_t& result) {
    if (!*token) return false;
    uint32_t value = 0;
    for (const char* p = token; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (digit > maximum || value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
    }
    result = value;
    return true;
}

bool uuidValid(const char* value) {
    if (strlen(value) != 36) return false;
    bool nonzero = false;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (value[i] >= '0' && value[i] <= '9') {
            if (value[i] != '0') nonzero = true;
        } else if (value[i] >= 'a' && value[i] <= 'f') nonzero = true;
        else return false;
    }
    return nonzero;
}

bool hostValid(const char* host) {
    const size_t length = strlen(host);
    if (!length || host[0] == '.' || host[length - 1] == '.' || host[0] == '-' || host[length - 1] == '-') return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        if (c == '.' && (host[i + 1] == '.' || host[i + 1] == '-')) return false;
        if (c == '-' && host[i + 1] == '.') return false;
    }
    return true;
}

bool certificateValid(const char* pem) {
    if (strncmp(pem, "-----BEGIN CERTIFICATE-----", 27) ||
        !strstr(pem, "-----END CERTIFICATE-----")) return false;
#ifdef ARDUINO_ARCH_ESP32
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    const int result = mbedtls_x509_crt_parse(&cert,
        reinterpret_cast<const unsigned char*>(pem), strlen(pem) + 1);
    mbedtls_x509_crt_free(&cert);
    return result == 0;
#else
    return true;
#endif
}

bool valid(const DeviceProvisioning::Settings& s) {
    if (!textValid(s.deviceId, sizeof(s.deviceId)) || !uuidValid(s.deviceId) ||
        s.deviceNumber <= 0 ||
        !textValid(s.secret, sizeof(s.secret)) || strlen(s.secret) != 64 ||
        !textValid(s.wifiSsid, sizeof(s.wifiSsid)) || !*s.wifiSsid ||
        !textValid(s.wifiPassword, sizeof(s.wifiPassword)) ||
        !textValid(s.backendHost, sizeof(s.backendHost)) || !hostValid(s.backendHost) ||
        !s.backendPort || s.tls > 1 || !textValid(s.caCert, sizeof(s.caCert), true)) return false;
    for (const char* p = s.secret; *p; ++p) if (hexValue(*p) < 0) return false;
    const size_t passwordLength = strlen(s.wifiPassword);
    if (passwordLength && (passwordLength < 8 || (passwordLength > 63 &&
        (passwordLength != 64 || strspn(s.wifiPassword, "0123456789abcdefABCDEF") != 64)))) return false;
    return s.tls ? certificateValid(s.caCert) : !*s.caCert;
}

uint32_t checksum(const Record& value) {
    uint32_t crc = 0xffffffffu;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < offsetof(Record, checksum); ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

bool recordValid(const Record& value) {
    return value.magic == Magic && value.version == SchemaVersion &&
        value.checksum == checksum(value) && valid(value.settings);
}

bool parse(const char* fields, DeviceProvisioning::Settings& settings) {
    const size_t length = boundedLength(fields, MaxFields);
    if (!length || length == MaxFields) return false;
    memcpy(input, fields, length + 1);
    char* cursor = nullptr;
    char* tokens[9];
    for (char*& token : tokens) {
        token = strtok_r(cursor ? nullptr : input, " ", &cursor);
        if (!token) return false;
    }
    if (strtok_r(nullptr, " ", &cursor)) return false;
    uint32_t number, port, tls;
    if (!copyToken(tokens[0], settings.deviceId, sizeof(settings.deviceId)) ||
        !unsignedNumber(tokens[1], INT32_MAX, number) ||
        !copyToken(tokens[2], settings.secret, sizeof(settings.secret)) ||
        !decodeHex(tokens[3], settings.wifiSsid, sizeof(settings.wifiSsid)) ||
        !decodeHex(tokens[4], settings.wifiPassword, sizeof(settings.wifiPassword)) ||
        !decodeHex(tokens[5], settings.backendHost, sizeof(settings.backendHost)) ||
        !unsignedNumber(tokens[6], UINT16_MAX, port) ||
        !unsignedNumber(tokens[7], 1, tls) ||
        !decodeHex(tokens[8], settings.caCert, sizeof(settings.caCert))) return false;
    settings.deviceNumber = static_cast<int32_t>(number);
    settings.backendPort = static_cast<uint16_t>(port);
    settings.tls = static_cast<uint8_t>(tls);
    return valid(settings);
}

DeviceProvisioning::Result invalidReason(const DeviceProvisioning::Settings& settings) {
    return settings.tls && *settings.caCert && !certificateValid(settings.caCert)
        ? DeviceProvisioning::Result::InvalidCertificate : DeviceProvisioning::Result::Invalid;
}
}

bool DeviceProvisioning::load() {
    configured_ = false;
    storageReady_ = false;
    writeUncertain_ = false;
    activeSlot_ = 0;
    generation_ = 0;
    memset(&settings_, 0, sizeof(settings_));
    Preferences prefs;
    // Read-only open fails on a fresh ESP32 because the namespace does not yet
    // exist. A read-write open creates only the namespace, not a valid record.
    if (!prefs.begin(Namespace, false)) return false;
    storageReady_ = true;
    const bool loaded = prefs.getBytesLength(Key) == sizeof(record) &&
        prefs.getBytes(Key, &record, sizeof(record)) == sizeof(record) && recordValid(record);
    const bool otherLoaded = prefs.getBytesLength(AlternateKey) == sizeof(alternate) &&
        prefs.getBytes(AlternateKey, &alternate, sizeof(alternate)) == sizeof(alternate) && recordValid(alternate);
    prefs.end();
    if (otherLoaded && (!loaded || static_cast<int16_t>(alternate.reserved - record.reserved) > 0)) {
        settings_ = alternate.settings; generation_ = alternate.reserved; activeSlot_ = 1; configured_ = true;
    } else if (loaded) {
        settings_ = record.settings; generation_ = record.reserved; configured_ = true;
    }
    return true;
}

DeviceProvisioning::Result DeviceProvisioning::configure(const char* fields) {
    if (configured_) return Result::Locked;
    if (!storageReady_ || writeUncertain_) return Result::StorageError;
    memset(&record, 0, sizeof(record));
    if (!fields || !parse(fields, record.settings)) return invalidReason(record.settings);
    record.magic = Magic;
    record.version = SchemaVersion;
    record.checksum = checksum(record);
    Preferences prefs;
    if (!prefs.begin(Namespace, false)) return Result::StorageError;
    const bool written = prefs.putBytes(Key, &record, sizeof(record)) == sizeof(record);
    const bool verified = written && prefs.getBytesLength(Key) == sizeof(record) &&
        prefs.getBytes(Key, &record, sizeof(record)) == sizeof(record) && recordValid(record);
    prefs.end();
    if (!verified) { writeUncertain_ = true; return Result::StorageError; }
    settings_ = record.settings;
    configured_ = true;
    activeSlot_ = 0;
    generation_ = 0;
    return Result::Saved;
}

DeviceProvisioning::Result DeviceProvisioning::update(const char* fields) {
    if (!configured_) return Result::Invalid;
    if (!storageReady_ || writeUncertain_) return Result::StorageError;
    memset(&record, 0, sizeof(record));
    if (!fields || !parse(fields, record.settings)) return invalidReason(record.settings);
    if (strcmp(record.settings.deviceId, settings_.deviceId) ||
        record.settings.deviceNumber != settings_.deviceNumber) return Result::Invalid;
    if (!memcmp(&record.settings, &settings_, sizeof(settings_))) return Result::Saved;
    record.magic = Magic;
    record.version = SchemaVersion;
    record.reserved = static_cast<uint16_t>(generation_ + 1);
    record.checksum = checksum(record);
    const char* target = activeSlot_ ? Key : AlternateKey;
    Preferences prefs;
    if (!prefs.begin(Namespace, false)) return Result::StorageError;
    const bool written = prefs.putBytes(target, &record, sizeof(record)) == sizeof(record);
    const bool verified = written && prefs.getBytesLength(target) == sizeof(record) &&
        prefs.getBytes(target, &record, sizeof(record)) == sizeof(record) && recordValid(record);
    prefs.end();
    if (!verified) { writeUncertain_ = true; return Result::StorageError; }
    settings_ = record.settings;
    generation_ = record.reserved;
    activeSlot_ = activeSlot_ ? 0 : 1;
    return Result::Saved;
}
