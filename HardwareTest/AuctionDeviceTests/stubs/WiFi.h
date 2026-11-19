#pragma once
#include "Arduino.h"
#include <sstream>

#define WL_CONNECTED 3
extern bool wifiConnected, backendReachable, tlsValid;
extern int httpStatus;
extern std::string lastHttpRequest;

struct WiFiType {
    int status() const { return wifiConnected ? WL_CONNECTED : 0; }
    void disconnect() {}
    void begin(const char*, const char*) {}
};
extern WiFiType WiFi;
inline void configTime(long, int, const char*, const char*) {}

class Client {
public:
    virtual ~Client() = default;
    void setTimeout(int) {}
    template<class T> size_t print(const T& value) {
        std::ostringstream stream; stream << value;
        const std::string text = stream.str();
        lastHttpRequest += text;
        return text.size();
    }
    String readStringUntil(char) {
        return "HTTP/1.1 " + std::to_string(httpStatus) + " Test\r";
    }
    void stop() {}
};

class WiFiClient : public Client {
public:
    bool connect(const char*, uint16_t, int) { return backendReachable; }
};
