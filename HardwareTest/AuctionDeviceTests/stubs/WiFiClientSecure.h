#pragma once
#include "WiFi.h"

class WiFiClientSecure : public Client {
public:
    void setCACert(const char*) {}
    bool connect(const char*, uint16_t, int) { return backendReachable && tlsValid; }
};
