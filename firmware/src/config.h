#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// DTRDesk.com Hardware Firmware & Pinout Configuration
// ============================================================================

#include "build_config.h"

// 0.96" I2C OLED Display Configuration (128x64 SSD1306)
#define SCREEN_WIDTH          128
#define SCREEN_HEIGHT         64
#define OLED_RESET            -1    // Reset pin # (or -1 if sharing Arduino reset pin)
#define OLED_I2C_ADDRESS      0x3C  // Common I2C address for 0.96" OLED
#define OLED_SCL_PIN          D1    // GPIO5 -> OLED SCL
#define OLED_SDA_PIN          D2    // GPIO4 -> OLED SDA

// AS608 Fingerprint Sensor Pin Definitions (SoftwareSerial)
#define FINGERPRINT_RX_PIN    D5    // GPIO14 -> AS608 TX (ESP8266 Serial RX)
#define FINGERPRINT_TX_PIN    D6    // GPIO12 -> AS608 RX (ESP8266 Serial TX)

// Hardware Indicator Pins (NodeMCU ESP8266)
#define LED_GREEN_PIN         D7    // GPIO13 -> Green LED (Access Granted)
#define LED_RED_PIN           D8    // GPIO15 -> Red LED (Access Denied / Offline)
#define BUZZER_PIN            D3    // GPIO0  -> Buzzer Feedback

// Timing & Debounce Parameters
#define SCAN_DEBOUNCE_MS      3000   // 3 Seconds debounce between identical scans
#define HEARTBEAT_INTERVAL_MS 30000  // 30 Seconds device sync heartbeat
#define RETRY_FLUSH_INTERVAL  15000  // 15 Seconds offline log flush check

#endif // CONFIG_H
