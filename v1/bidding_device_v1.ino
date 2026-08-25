#include <LovyanGFX.hpp>

// ============================================================
// CYD DISPLAY CONFIGURATION
// ESP32-2432S028R + ILI9341
// ============================================================

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {

    // --------------------------------------------------------
    // SPI BUS
    // --------------------------------------------------------
    {
      auto cfg = _bus.config();

      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;

      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;

      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = 1;

      // CYD display pins
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    // --------------------------------------------------------
    // ILI9341 DISPLAY
    // --------------------------------------------------------
    {
      auto cfg = _panel.config();

      cfg.pin_cs   = 15;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;

      cfg.panel_width  = 240;
      cfg.panel_height = 320;

      _panel.config(cfg);
    }

    setPanel(&_panel);
  }
};


// ============================================================
// HARDWARE
// ============================================================

LGFX display;

// Physical buttons
const int BID_BUTTON  = 22;
const int PLUS_BUTTON = 27;

// CYD backlight
const int BACKLIGHT = 21;


// ============================================================
// BUTTON STATE
// ============================================================

bool previousBidState  = HIGH;
bool previousPlusState = HIGH;


// ============================================================
// AUCTION STATE
// ============================================================

// Current accepted bid
int currentBid = 5000;

// Bid currently selected by the user
int yourBid = 5500;

// Temporary fixed bid increment
const int bidIncrement = 500;


// ============================================================
// DRAW AUCTION SCREEN
// ============================================================

void drawAuctionScreen() {

  display.fillScreen(TFT_BLACK);

  display.setTextColor(TFT_WHITE);

  // --------------------------------------------------------
  // Header
  // --------------------------------------------------------

  display.setTextSize(2);
  display.setCursor(55, 15);
  display.println("AUCTION DEVICE");

  display.drawFastHLine(
    20,
    45,
    280,
    TFT_WHITE
  );


  // --------------------------------------------------------
  // Current bid
  // --------------------------------------------------------

  display.setTextSize(2);
  display.setCursor(20, 65);
  display.println("CURRENT BID");

  display.setTextSize(3);
  display.setCursor(20, 90);
  display.printf("%d kr", currentBid);


  // --------------------------------------------------------
  // Selected bid
  // --------------------------------------------------------

  display.setTextSize(2);
  display.setCursor(20, 140);
  display.println("YOUR BID");

  display.setTextSize(3);
  display.setCursor(20, 165);
  display.printf("%d kr", yourBid);


  // --------------------------------------------------------
  // Status
  // --------------------------------------------------------

  display.setTextSize(2);
  display.setCursor(125, 215);
  display.println("READY");
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  Serial.println();
  Serial.println("Starting auction device...");


  // --------------------------------------------------------
  // Configure physical buttons
  // --------------------------------------------------------

  pinMode(BID_BUTTON, INPUT_PULLUP);
  pinMode(PLUS_BUTTON, INPUT_PULLUP);


  // --------------------------------------------------------
  // Backlight
  // --------------------------------------------------------

  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, HIGH);


  // --------------------------------------------------------
  // Display
  // --------------------------------------------------------

  display.init();

  // Landscape orientation
  display.setRotation(1);

  drawAuctionScreen();


  Serial.println("Auction device ready.");
  Serial.println();

  Serial.print("Current bid: ");
  Serial.println(currentBid);

  Serial.print("Selected bid: ");
  Serial.println(yourBid);
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  // Read physical buttons
  bool currentBidState =
    digitalRead(BID_BUTTON);

  bool currentPlusState =
    digitalRead(PLUS_BUTTON);


  // ========================================================
  // PLUS BUTTON
  //
  // Detect HIGH -> LOW transition.
  // This means the button has just been pressed.
  // ========================================================

  if (
    previousPlusState == HIGH &&
    currentPlusState == LOW
  ) {

    yourBid += bidIncrement;

    Serial.print("Selected bid changed to: ");
    Serial.println(yourBid);

    drawAuctionScreen();
  }


  // ========================================================
  // BID BUTTON
  //
  // For now we simulate that the server accepts the bid.
  // ========================================================

  if (
    previousBidState == HIGH &&
    currentBidState == LOW
  ) {

    // Accept selected bid
    currentBid = yourBid;

    // Prepare minimum next bid
    yourBid = currentBid + bidIncrement;


    Serial.print("BID ACCEPTED: ");
    Serial.println(currentBid);

    Serial.print("Next minimum bid: ");
    Serial.println(yourBid);


    drawAuctionScreen();
  }


  // ========================================================
  // Store button states for next loop
  // ========================================================

  previousBidState = currentBidState;
  previousPlusState = currentPlusState;
}