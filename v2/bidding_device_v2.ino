#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID "input wifi name here"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6

#define API_HOST "api host ip address"
#define API_PORT 1111
#define API_PATH "websocket endpoint in api project"

#define SDA 13
#define SCL 14
#define PIN_ANALOG_IN 4
#define PIN_BID_BUTTON 5

LiquidCrystal_I2C lcd(0x27, 16, 2);
WebSocketsClient webSocket;

bool auctionActive = false;
int auctionItemId = 0;
String auctionItem = "";
int currentBid = 0;
int customerBid = 0;

void wiFiSetup(void);
void webSocketSetup(void);
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void handleServerMessage(uint8_t * payload);
void updateAuctionDisplay(void);
void updateCustomerBid(void);
void sendBid(void);
bool i2CAddrTest(uint8_t addr);

void setup() {
  Serial.begin(115200);

  Wire.begin(SDA, SCL);
  if (!i2CAddrTest(0x27)) {
    lcd = LiquidCrystal_I2C(0x3, 16, 2);
  }
  lcd.init();
  lcd.backlight();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting...");

  pinMode(PIN_BID_BUTTON, INPUT_PULLUP);

  wiFiSetup();
  webSocketSetup();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connected");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());

  delay(2000);

  lcd.clear();
}

void loop() {
  webSocket.loop();
  updateCustomerBid();
  updateAuctionDisplay();

  if (digitalRead(PIN_BID_BUTTON) == 0) {
    sendBid();
    delay(300);
  }

  delay(50);
}

void wiFiSetup(void) {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  Serial.print("Connecting to WiFi: ");
  Serial.print(WIFI_SSID);

  // wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" Connected");
  Serial.print("IP-address: ");
  Serial.print(WiFi.localIP());
}

void webSocketSetup(void) {
  Serial.println("Connectiong to WebSocket server...");

  webSocket.begin(API_HOST, API_PORT, API_PATH);

  webSocket.onEvent(webSocketEvent);

  // attempt reconnect every 5 seconds
  webSocket.setReconnectInterval(5000);
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    Serial.println("WebSocket connected!");
    // Tell server that this is an ESP32
    webSocket.sendTXT("{\"type\":\"register\",\"client\":\"esp32\"}");
    break;

  case WStype_DISCONNECTED:
    Serial.println("WebSocket disconnected");
    break;

  case WStype_TEXT:
    Serial.print("Received from server: ");
    Serial.println((char * ) payload);
    handleServerMessage(payload);
    break;

  default:
    break;
  }
}

void handleServerMessage(uint8_t * payload) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());
    return;
  }

  const char * type = doc["type"];

  if (type == nullptr) {
    Serial.println("Message has no type");
    return;
  }

  if (strcmp(type, "auctionStarted") == 0) {
    auctionActive = true;
    auctionItemId = doc["type"] | 0;
    auctionItem = doc["itemName"].as < String > ();
    currentBid = doc["currentBid"] | 0;

    Serial.println();
    Serial.println(" === Auction Started === ");

    Serial.print("Item ID: ");
    Serial.println(auctionItemId);
    Serial.print("Item: ");
    Serial.println(auctionItem);
    Serial.print("Starting bid: ");
    Serial.println(currentBid);
    Serial.println();
  } else if (strcmp(type, "bidUpdated") == 0) {
    currentBid = doc["currentBid"] | currentBid;
    Serial.print("Current bid updated: ");
    Serial.println(currentBid);
  } else if (strcmp(type, "auctionEnded") == 0) {
    auctionActive = false;
    Serial.println();
    Serial.println("=== AUCTION ENDED ===");
    Serial.println();
  } else {
    Serial.println("Unknown message type: ");
    Serial.println(type);
  }
}

void updateAuctionDisplay(void) {
  lcd.setCursor(0, 0);
  if (auctionActive) {
    lcd.print("Cur bid:");
    lcd.print(currentBid);
  } else {
    lcd.print("No auction active");
  }
}

void updateCustomerBid(void) {
  int potValue = analogRead(PIN_ANALOG_IN);

  //Serial.print("Pot val: ");
  //Serial.print(potValue);
  //Serial.print("\n");

  int customerBid = map(potValue, 0, 4095, 1000, 5000);

  lcd.setCursor(0, 1);
  lcd.print("Your bid:");
  lcd.print(customerBid);
}

void sendBid(void) {
  // don't submit bids if no auction is active
  if (!auctionActive) {
    Serial.println("Cannot bid: no auction active");
    return;
  }

  // make sure websocket is connected
  if (!webSocket.isConnected()) {
    Serial.println("Cannot bid: WebSocket disconnected");
    return;
  }

  // create json
  JsonDocument doc;
  doc["type"] = "placeBid";
  doc["amount"] = customerBid;

  // serialize json
  String message;
  serializeJson(doc, message);

  // send
  webSocket.sendTXT(message);

  Serial.print("Bid sent: ");
  Serial.print(customerBid);
}

bool i2CAddrTest(uint8_t addr) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() == 0) {
    return true;
  }
  return false;
}