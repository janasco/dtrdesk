#ifndef DTRDESK_DISPLAY_H
#define DTRDESK_DISPLAY_H
//
// Display abstraction for the DTRDesk gateway.
//
// Two options, chosen at build time:
//   default                      -> 0.96" SSD1306 128x64 OLED over I2C (D1/D2)
//   -D DTRDESK_DISPLAY_ST7789=1  -> 2.8" ST7789 240x320 SPI TFT (landscape)
//
// The TFT option retires the OLED and reuses its pins for the SPI bus and the
// fingerprint reader (see config.h) — a NodeMCU only exposes 9 usable GPIOs.
// TFT_eSPI is a Print subclass, so print()/setCursor()/setTextSize() behave like
// Adafruit_GFX; clearDisplay()/display() keep the SSD1306 call sites working.
//
#ifdef DTRDESK_DISPLAY_ST7789
  #include <TFT_eSPI.h>

  class DtrDisplay : public TFT_eSPI {
  public:
    bool begin() {
      init();
      setRotation(1); // landscape 320x240
      return true;
    }
    void clearDisplay() { fillScreen(TFT_BLACK); }
    void display() {} // TFT_eSPI draws directly; kept for API parity
  };

  static DtrDisplay display;
  static inline void displayBusInit() {}
  static inline bool displayBegin() { return display.begin(); }

  #ifndef SSD1306_WHITE
    #define SSD1306_WHITE TFT_WHITE
    #define SSD1306_BLACK TFT_BLACK
  #endif
#else
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>

  static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  static inline void displayBusInit() { Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN); }
  static inline bool displayBegin() { return display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS); }
#endif

#endif // DTRDESK_DISPLAY_H
