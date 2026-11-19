#include <cstdio>
#include <cstdlib>
#include <string>
#include "stubs/Preferences.h"
#include "../AuctionDevice/DeviceProvisioning.h"

int checks = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
    std::printf("PASS: %s\n", name);
}

std::string hex(const std::string& value) {
    const char* digits = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : value) { output += digits[c >> 4]; output += digits[c & 15]; }
    return output;
}

std::string fields(const std::string& id = "11111111-1111-4111-8111-111111111111",
                   const std::string& number = "42", const std::string& secret = std::string(64, 'A'),
                   const std::string& password = "password1", const std::string& host = "backend.test",
                   const std::string& tls = "0", const std::string& cert = "-") {
    return id + " " + number + " " + secret + " " + hex("AuctionWifi") + " " + hex(password) +
        " " + hex(host) + " 443 " + tls + " " + cert;
}

int main() {
    DeviceProvisioning device;
    check(device.load() && !device.configured(), "empty NVS awaits provisioning");
    check(device.configure(fields("not-a-uuid").c_str()) == DeviceProvisioning::Result::Invalid,
          "invalid UUID rejected before persistence");
    check(device.configure(fields("11111111-1111-4111-8111-111111111111", "0").c_str()) == DeviceProvisioning::Result::Invalid,
          "invalid device number rejected");
    check(device.configure(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'Z')).c_str()) == DeviceProvisioning::Result::Invalid,
          "invalid secret rejected");
    check(device.configure(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'A'), "short").c_str()) == DeviceProvisioning::Result::Invalid,
          "short Wi-Fi password rejected");
    check(device.configure(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'A'), "password1", "https://backend.test").c_str()) == DeviceProvisioning::Result::Invalid,
          "backend scheme in host rejected");
    check(device.configure(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'A'), "password1", "backend.test", "1").c_str()) == DeviceProvisioning::Result::Invalid,
          "TLS requires a certificate");
    check(device.configure(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'A'),
          "password1", "backend.test", "1", hex("-----BEGIN CERTIFICATE-----\ninvalid")).c_str()) ==
          DeviceProvisioning::Result::InvalidCertificate, "malformed certificate gets a safe error");
    check(device.configure((fields() + " extra").c_str()) == DeviceProvisioning::Result::Invalid,
          "extra fields rejected");
    check(nvsBlob.empty(), "invalid input never persists");

    nvsWriteLimit = 100;
    check(device.configure(fields().c_str()) == DeviceProvisioning::Result::StorageError,
          "interrupted NVS write is not activated");
    nvsWriteLimit = SIZE_MAX;
    check(device.configure(fields("22222222-2222-4222-8222-222222222222").c_str()) == DeviceProvisioning::Result::StorageError,
          "uncertain NVS write blocks another identity until reboot");
    DeviceProvisioning afterInterruption;
    check(afterInterruption.load() && !afterInterruption.configured(), "partial NVS blob is not bootable");
    check(afterInterruption.configure(fields().c_str()) == DeviceProvisioning::Result::Saved,
          "complete configuration is saved");
    DeviceProvisioning rebooted;
    check(rebooted.load() && rebooted.configured(), "complete configuration survives reboot");
    check(std::string(rebooted.settings().deviceId) == "11111111-1111-4111-8111-111111111111" &&
          rebooted.settings().deviceNumber == 42 && std::string(rebooted.settings().secret) == std::string(64, 'A') &&
          std::string(rebooted.settings().wifiPassword) == "password1" &&
          std::string(rebooted.settings().backendHost) == "backend.test" && rebooted.settings().backendPort == 443,
          "identity, credentials and network settings survive reboot");
    check(rebooted.configure(fields("22222222-2222-4222-8222-222222222222").c_str()) == DeviceProvisioning::Result::Locked,
          "normal connection cannot overwrite configuration");
    check(std::string(rebooted.settings().deviceId) == "11111111-1111-4111-8111-111111111111",
          "locked configuration keeps original identity");
    nvsBlob[30] ^= 1;
    DeviceProvisioning corrupted;
    check(corrupted.load() && !corrupted.configured(), "corrupted NVS record is rejected");
    nvsBlob.clear();
    DeviceProvisioning repairBase;
    check(repairBase.load() && repairBase.configure(fields().c_str()) == DeviceProvisioning::Result::Saved,
          "repair fixture has a complete original configuration");
    nvsWriteLimit = 100;
    check(repairBase.update(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'B')).c_str()) == DeviceProvisioning::Result::StorageError,
          "interrupted update is not activated");
    check(std::string(repairBase.settings().secret) == std::string(64, 'A'), "failed update retains live old credential");
    DeviceProvisioning repairReboot;
    check(repairReboot.load() && std::string(repairReboot.settings().secret) == std::string(64, 'A'),
          "power loss during update boots previous complete configuration");
    nvsWriteLimit = SIZE_MAX;
    check(repairReboot.update(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'B')).c_str()) == DeviceProvisioning::Result::Saved,
          "explicit update saves new credential on alternate NVS slot");
    check(repairReboot.credentialVersion() == 2, "completed update advances credential version");
    const auto alternateAfterUpdate = nvsAlternateBlob;
    check(repairReboot.update(fields("11111111-1111-4111-8111-111111111111", "42", std::string(64, 'B')).c_str()) == DeviceProvisioning::Result::Saved &&
          repairReboot.credentialVersion() == 2 && nvsAlternateBlob == alternateAfterUpdate,
          "identical retry after lost acknowledgement does not rewrite or advance version");
    DeviceProvisioning repaired;
    check(repaired.load() && std::string(repaired.settings().secret) == std::string(64, 'B'),
          "complete update survives restart");
    nvsAlternateBlob[30] ^= 1;
    DeviceProvisioning damagedRepair;
    check(damagedRepair.load() && std::string(damagedRepair.settings().secret) == std::string(64, 'A'),
          "corrupt updated slot falls back to previous complete configuration");
    nvsBlob.clear(); nvsAlternateBlob.clear();
    DeviceProvisioning tlsDevice;
    const std::string pem = "-----BEGIN CERTIFICATE-----\nQUJD\n-----END CERTIFICATE-----\n";
    check(tlsDevice.load() && tlsDevice.configure(fields("11111111-1111-4111-8111-111111111111", "42",
          std::string(64, 'A'), "password1", "backend.test", "1", hex(pem)).c_str()) == DeviceProvisioning::Result::Saved,
          "TLS mode and PEM trust anchor are stored together");
    DeviceProvisioning tlsRebooted;
    check(tlsRebooted.load() && tlsRebooted.settings().tls == 1 && tlsRebooted.settings().caCert == pem,
          "TLS settings survive reboot");
    nvsAvailable = false;
    check(!corrupted.load() && corrupted.configure(fields().c_str()) == DeviceProvisioning::Result::StorageError,
          "NVS failure never activates configuration");
    std::printf("%d provisioning checks passed\n", checks);
}
