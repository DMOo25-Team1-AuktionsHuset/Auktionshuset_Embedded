#pragma once

#include <stdint.h>

// One complete, versioned NVS record is written only after every field validates.
class DeviceProvisioning {
public:
    struct Settings {
        char deviceId[37];
        int32_t deviceNumber;
        char secret[65];
        char wifiSsid[33];
        char wifiPassword[65];
        char backendHost[254];
        uint16_t backendPort;
        uint8_t tls;
        char caCert[3072];
    };

    enum class Result { Saved, Locked, Invalid, InvalidCertificate, StorageError };

    bool load();
    bool configured() const { return configured_; }
    const Settings& settings() const { return settings_; }
    uint16_t credentialVersion() const { return static_cast<uint16_t>(generation_ + 1); }
    // Nine space-separated fields after "AHCFG/1 SET "; text fields are hex encoded.
    Result configure(const char* fields);
    // Explicit replacement uses the other NVS slot; a failed write keeps the last valid record.
    Result update(const char* fields);

private:
    Settings settings_{};
    bool configured_ = false;
    bool storageReady_ = false;
    bool writeUncertain_ = false;
    uint8_t activeSlot_ = 0;
    uint16_t generation_ = 0;
};
