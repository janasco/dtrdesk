#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

#include "config.h"
#include "LittleFSBuffer.h"

// Hardware Interfaces
SoftwareSerial mySerial(FINGERPRINT_RX_PIN, FINGERPRINT_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// State tracking variables
unsigned long lastScanTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastFlushTime = 0;
int lastScannedSlot = -1;
bool isOledConnected = false;
bool isLockdownActive = false;

// Function Prototypes
void setupWiFi();
void triggerFeedback(bool success, int beepCount = 1);
int readFingerprintID();
bool transmitBiometricLog(int slotId, unsigned long timestamp, bool isOfflineBuff = false);
void checkHeartbeat();
void updateOledStatus(const char* title, const char* msg1, const char* msg2 = "", bool isSuccess = true);
bool enrollFingerprint(int slotId);

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=============================================="));
    Serial.println(F("    DTRDesk.com ESP8266 Biometric Firmware   "));
    Serial.println(F("=============================================="));

    // Initialize Pin Modes
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    // Initialize I2C Wire & OLED Display (SCL=D1, SDA=D2)
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
        isOledConnected = true;
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        updateOledStatus("DTRDesk.com", "Initializing...", "Booting System");
        delay(1000);
    } else {
        Serial.println(F("[OLED] Warning: 0.96\" SSD1306 OLED display not found on I2C (0x3C)."));
    }

    // Mount LittleFS Flash File System
    LittleFSBuffer::begin();

    // Initialize Fingerprint Sensor
    finger.begin(57600);
    if (finger.verifyPassword()) {
        Serial.println(F("[Hardware] AS608 Fingerprint Sensor Detected Successfully!"));
        updateOledStatus("DTRDesk.com", "AS608 Sensor", "Ready Status: OK");
        triggerFeedback(true, 2); // Double beep hardware startup
    } else {
        Serial.println(F("[Hardware] ERROR: Fingerprint sensor not found. Check wiring!"));
        updateOledStatus("DTRDesk.com", "Sensor Error!", "AS608 Disconnected", false);
        triggerFeedback(false, 3);
    }

    // Connect to Wi-Fi
    setupWiFi();
    updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
}

void loop() {
    unsigned long currentMillis = millis();

    // 1. Maintain Wi-Fi Connection
    if (WiFi.status() != WL_CONNECTED && currentMillis % 10000 < 50) {
        Serial.println(F("[WiFi] Reconnecting to network..."));
        WiFi.reconnect();
    }

    // 2. Scan Fingerprint Sensor (Non-blocking check)
    if (!isLockdownActive) {
        int slotId = readFingerprintID();
        if (slotId > 0) {
            // Debounce: Avoid rapid repeat triggers for same slot
            if (slotId != lastScannedSlot || (currentMillis - lastScanTime) > SCAN_DEBOUNCE_MS) {
                lastScannedSlot = slotId;
                lastScanTime = currentMillis;

                Serial.printf("\n[Biometric] Match Found! Slot ID: #%d\n", slotId);
                char slotBuf[32];
                snprintf(slotBuf, sizeof(slotBuf), "Slot #%d Matched", slotId);
                updateOledStatus("ACCESS GRANTED", slotBuf, "Ingesting to Edge...");

                unsigned long timestamp = millis() / 1000;
                bool transmitted = transmitBiometricLog(slotId, timestamp, false);

                if (transmitted) {
                    Serial.println(F("[Sync] Real-time log transmitted to Cloud API successfully."));
                    updateOledStatus("ACCESS GRANTED", "Clock Recorded!", "Sync: Direct Edge", true);
                    triggerFeedback(true, 1); // Single positive beep
                } else {
                    Serial.println(F("[Sync] Server unreachable. Buffering log to LittleFS flash memory."));
                    LittleFSBuffer::appendLog(slotId, timestamp);
                    updateOledStatus("OFFLINE LOGGED", "Saved to Flash", "Sync Pending...", true);
                    triggerFeedback(true, 2);
                }

                delay(1200); // Visual pause on OLED
                updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
            }
        }
    }

    // 3. Periodic Background Flush of LittleFS Buffer (Every 15s)
    if (currentMillis - lastFlushTime > RETRY_FLUSH_INTERVAL) {
        lastFlushTime = currentMillis;

        if (WiFi.status() == WL_CONNECTED && LittleFSBuffer::hasPendingLogs()) {
            Serial.println(F("\n[LittleFS] Pending offline logs detected. Starting auto-flush..."));
            std::vector<LittleFSBuffer::OfflineLog> pendingList = LittleFSBuffer::getPendingLogs();

            bool allSucceeded = true;
            for (const auto& logItem : pendingList) {
                bool ok = transmitBiometricLog(logItem.slotId, logItem.timestamp, true);
                if (!ok) {
                    allSucceeded = false;
                    Serial.println(F("[LittleFS] Flush paused. Network connection unstable."));
                    break;
                }
                delay(100);
            }

            if (allSucceeded) {
                LittleFSBuffer::clearBuffer();
                Serial.println(F("[LittleFS] All buffered logs successfully synchronized with Cloud!"));
                updateOledStatus("BUFFER FLUSHED", "All Offline Logs", "Synced to Edge", true);
                delay(1000);
                updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
            }
        }
    }

    // 4. Device Telemetry Heartbeat (Every 30s)
    if (currentMillis - lastHeartbeatTime > HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatTime = currentMillis;
        checkHeartbeat();
    }
}

/**
 * Render Status Screen to 0.96" I2C OLED (128x64 SSD1306)
 */
void updateOledStatus(const char* title, const char* msg1, const char* msg2, bool isSuccess) {
    if (!isOledConnected) return;
    display.clearDisplay();

    // Top Header Bar
    display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(4, 3);
    display.print(title);

    // Body Status Lines
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 22);
    display.setTextSize(1);
    display.print(msg1);

    if (msg2 && strlen(msg2) > 0) {
        display.setCursor(4, 38);
        display.setTextSize(1);
        display.print(msg2);
    }

    // Bottom Indicator Line
    display.drawLine(0, 52, SCREEN_WIDTH, 52, SSD1306_WHITE);
    display.setCursor(4, 55);
    display.setTextSize(1);
    if (WiFi.status() == WL_CONNECTED) {
        display.print(F("WIFI: OK | ID: GATE_01"));
    } else {
        display.print(F("OFFLINE | FLASH BUFFER"));
    }

    display.display();
}

/**
 * Setup and Connect Wi-Fi
 */
void setupWiFi() {
    Serial.printf("\n[WiFi] Connecting to %s...\n", WIFI_SSID);
    updateOledStatus("DTRDesk.com", "Connecting WiFi...", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(F("."));
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_RED_PIN, LOW);
        Serial.println(F("\n[WiFi] Connected successfully!"));
        Serial.print(F("[WiFi] IP Address: "));
        Serial.println(WiFi.localIP());
        updateOledStatus("DTRDesk.com", "WiFi Connected!", WiFi.localIP().toString().c_str());
        delay(1000);
    } else {
        Serial.println(F("\n[WiFi] Connection timed out. Operating in Offline Mode."));
        digitalWrite(LED_RED_PIN, HIGH);
        updateOledStatus("DTRDesk.com", "Offline Mode", "Flash Buffer Active", false);
        delay(1000);
    }
}

/**
 * Hardware Feedback (LED & Buzzer Indicator)
 */
void triggerFeedback(bool success, int beepCount) {
    int targetPin = success ? LED_GREEN_PIN : LED_RED_PIN;

    for (int i = 0; i < beepCount; i++) {
        digitalWrite(targetPin, HIGH);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(80);
        digitalWrite(targetPin, LOW);
        digitalWrite(BUZZER_PIN, LOW);
        if (i < beepCount - 1) delay(80);
    }
}

/**
 * Read Fingerprint Sensor Image & Search Template Database
 */
int readFingerprintID() {
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) return -1;

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return -1;

    p = finger.fingerFastSearch();
    if (p != FINGERPRINT_OK) {
        Serial.println(F("[Biometric] Unrecognized fingerprint scan."));
        updateOledStatus("SCAN DENIED", "Unrecognized Finger", "Access Denied", false);
        triggerFeedback(false, 2);
        delay(1200);
        updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
        return -1;
    }

    return finger.fingerID;
}

/**
 * Interactive Step-by-Step Biometric Enrollment Routine (Phase 3)
 */
bool enrollFingerprint(int slotId) {
    Serial.printf("[Enroll] Starting interactive enrollment for Slot #%d\n", slotId);
    
    // Step 1: Place Finger
    updateOledStatus("ENROLL MODE", "Place Finger", "Sensor Waiting...");
    triggerFeedback(true, 1);

    int p = -1;
    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        delay(50);
    }

    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
        updateOledStatus("ENROLL ERROR", "Image 1 Failed", "Try Again", false);
        triggerFeedback(false, 2);
        return false;
    }

    // Step 2: Remove Finger
    updateOledStatus("ENROLL MODE", "Remove Finger", "Lift Finger Now");
    delay(1000);
    p = 0;
    while (p != FINGERPRINT_NOFINGER) {
        p = finger.getImage();
        delay(50);
    }

    // Step 3: Place Same Finger Again
    updateOledStatus("ENROLL MODE", "Place Again", "Same Finger...");
    p = -1;
    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        delay(50);
    }

    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
        updateOledStatus("ENROLL ERROR", "Image 2 Failed", "Try Again", false);
        triggerFeedback(false, 2);
        return false;
    }

    // Step 4: Create & Store Model
    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        updateOledStatus("ENROLL ERROR", "Prints Do Not Match", "Try Again", false);
        triggerFeedback(false, 2);
        return false;
    }

    p = finger.storeModel(slotId);
    if (p == FINGERPRINT_OK) {
        char successBuf[32];
        snprintf(successBuf, sizeof(successBuf), "Slot #%d Saved!", slotId);
        updateOledStatus("ENROLL SUCCESS!", successBuf, "Registered to AS608", true);
        triggerFeedback(true, 2);
        delay(2000);
        updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
        return true;
    }

    updateOledStatus("ENROLL ERROR", "Storage Failed", "Memory Full", false);
    triggerFeedback(false, 2);
    return false;
}

/**
 * Post Biometric Scan Payload to Cloud API (POST /api/v1/device/logs)
 */
bool transmitBiometricLog(int slotId, unsigned long timestamp, bool isOfflineBuff) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClient client;
    HTTPClient http;

    http.begin(client, DTRDESK_API_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-DTRDesk-Device-Key", DTRDESK_DEVICE_KEY);
    http.setTimeout(4000);

    StaticJsonDocument<256> doc;
    doc["device_id"] = DTRDESK_DEVICE_ID;
    doc["fingerprint_slot_id"] = slotId;
    doc["raw_timestamp"] = timestamp;
    doc["is_offline_buff"] = isOfflineBuff;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload);
    bool success = (httpCode == 200 || httpCode == 201);

    if (success) {
        String response = http.getString();
        Serial.printf("[API Code %d] Response: %s\n", httpCode, response.c_str());
    } else {
        Serial.printf("[API Error] HTTP POST failed with code: %d\n", httpCode);
    }

    http.end();
    return success;
}

/**
 * Send Sync Heartbeat & Check Remote Commands from Cloud (Phase 3)
 */
void checkHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClient client;
    HTTPClient http;

    http.begin(client, DTRDESK_SYNC_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-DTRDesk-Device-Key", DTRDESK_DEVICE_KEY);
    http.setTimeout(3000);

    StaticJsonDocument<256> doc;
    doc["device_id"] = DTRDESK_DEVICE_ID;
    doc["firmware_version"] = DTRDESK_FIRMWARE_VERSION;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["flash_buffer_count"] = LittleFSBuffer::hasPendingLogs() ? 1 : 0;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload);
    if (httpCode == 200) {
        String resp = http.getString();
        StaticJsonDocument<256> respDoc;
        DeserializationError err = deserializeJson(respDoc, resp);
        if (!err) {
            const char* mode = respDoc["mode"] | "NORMAL";
            if (strcmp(mode, "ENROLL") == 0) {
                int targetSlot = respDoc["enroll_target_slot"] | 1;
                enrollFingerprint(targetSlot);
            }
        }
        Serial.println(F("[Heartbeat] Telemetry sync OK."));
    }
    http.end();
}
