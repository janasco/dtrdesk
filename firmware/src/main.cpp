#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>

#include "config.h"
#include "Display.h"
#include "DeviceConfig.h"
#include "LittleFSBuffer.h"
#include "WiFiManager.h"
#include "TrustAnchors.h"

// Earliest sane wall-clock epoch (2024-01-01T00:00:00Z). Anything below this
// means neither NTP nor the server epoch has been adopted yet.
#define MIN_VALID_EPOCH 1704067200UL

// Compile-time build epoch, used only to satisfy BearSSL's certificate date
// check when the system clock is not set yet. This lets the heartbeat adopt the
// server epoch without ever disabling certificate validation.
static time_t buildEpoch() {
    static time_t cached = 0;
    if (cached == 0) {
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* d = __DATE__; // "Mmm dd yyyy"
        char mon[4] = { d[0], d[1], d[2], 0 };
        const char* at = strstr(months, mon);
        struct tm tmv = {};
        tmv.tm_mon = at ? (int)(at - months) / 3 : 0;
        tmv.tm_mday = atoi(d + 4);
        tmv.tm_year = atoi(d + 7) - 1900;
        int hh = 0, mm = 0, ss = 0;
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
        tmv.tm_hour = hh;
        tmv.tm_min = mm;
        tmv.tm_sec = ss;
        cached = mktime(&tmv);
    }
    return cached;
}

// True once the clock is at/after 2024, i.e. safe to stamp attendance records.
static bool clockValid() {
    return time(nullptr) >= (time_t)MIN_VALID_EPOCH;
}

// Shared trust anchors. Parsed once and outlives every TLS client that uses it.
static BearSSL::X509List& trustAnchors() {
    static BearSSL::X509List anchors(DTRDESK_ROOT_CA_BUNDLE);
    return anchors;
}

// Enable certificate validation on an HTTPS client instead of setInsecure().
static void configureTls(WiFiClientSecure& client) {
    client.setTrustAnchors(&trustAnchors());
    time_t now = time(nullptr);
    if (now < (time_t)MIN_VALID_EPOCH) now = buildEpoch();
    client.setX509Time(now);
}

// Start SNTP and give it a bounded window to set the clock after Wi-Fi is up.
static void syncClockNtp(unsigned long timeoutMs = 10000) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print(F("[NTP] Waiting for time sync"));
    unsigned long start = millis();
    while (!clockValid() && (millis() - start) < timeoutMs) {
        delay(250);
        Serial.print('.');
        yield();
    }
    if (clockValid()) {
        Serial.printf("\n[NTP] Clock synchronized: %lu\n", (unsigned long)time(nullptr));
    } else {
        Serial.println(F("\n[NTP] Unavailable; will adopt server time on next heartbeat."));
    }
}

// Hardware Interfaces
SoftwareSerial mySerial(FINGERPRINT_RX_PIN, FINGERPRINT_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);
// The display object (SSD1306 or ST7789) lives in Display.h.

// State tracking variables
unsigned long lastScanTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastFlushTime = 0;
unsigned long lastClaimTime = 0;
int lastScannedSlot = -1;
bool isOledConnected = false;
bool isLockdownActive = false;
DTRDeskWiFiManager wifiManager;

// Sensor-fault state. Persisted so a wiring fault survives a reboot and is
// reported to the backend until the AS608 answers again.
static const char* SENSOR_FAULT_FILE = "/sensor_fault.json";
bool sensorFaulted = false;
String sensorFaultReason;

// Set when /device/logs or /device/heartbeat answers 401/403: the key was
// revoked or rotated. Unlike a network blip this is permanent, so we stop
// retrying and send the user back through the captive portal.
bool deviceRevoked = false;
bool revocationHandled = false;

// Last heartbeat outcome, surfaced by the serial self-test (`selftest`).
bool lastHeartbeatOk = false;
bool lastHeartbeatAttempted = false;

// Function Prototypes
// WiFi Manager handles connectivity
void triggerFeedback(bool success, int beepCount = 1);
int readFingerprintID();
bool transmitBiometricLog(int slotId, unsigned long timestamp, bool isOfflineBuff = false);
void checkHeartbeat();
void updateOledStatus(const char* title, const char* msg1, const char* msg2 = "", bool isSuccess = true, bool force = false);
bool enrollFingerprint(int slotId, const char* fingerName = "");
void reportEnrollmentResult(int slotId, bool success, const char* finger);
bool tryClaimGateway();
bool tryRegisterGateway();
void persistSensorFault(bool faulted, const String& reason);
void loadSensorFault();
void handleRevocation();
void printDiagnostics();

/**
 * Drive a status LED. The TFT pin map wires the red LED active-low so GPIO2
 * stays high at boot; every call site goes through here.
 */
static inline void ledWrite(int pin, bool on) {
#ifdef LED_RED_ACTIVE_LOW
    if (pin == LED_RED_PIN) {
        digitalWrite(pin, on ? LOW : HIGH);
        return;
    }
#endif
    digitalWrite(pin, on ? HIGH : LOW);
}

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
    ledWrite(LED_GREEN_PIN, false);
    ledWrite(LED_RED_PIN, false);
    digitalWrite(BUZZER_PIN, LOW);

    // Initialize the display bus + panel (OLED I2C or TFT SPI).
    displayBusInit();
    if (displayBegin()) {
        isOledConnected = true;
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        updateOledStatus("DTRDesk.com", "Initializing...", "Booting System");
        delay(1000);
    } else {
        Serial.println(F("[OLED] Warning: 0.96\" SSD1306 OLED display not found on I2C (0x3C)."));
    }

    // Mount LittleFS Flash File System, then load runtime device credentials
    LittleFSBuffer::begin();
    DeviceConfig::begin();
    Serial.printf("[Device] id=%s provisioned=%s usable_key=%s\n",
                  DeviceConfig::deviceId().c_str(),
                  DeviceConfig::isProvisioned() ? "yes" : "no",
                  DeviceConfig::hasUsableKey() ? "yes" : "no");

    // Initialize Fingerprint Sensor
    loadSensorFault();
    finger.begin(57600);
    if (finger.verifyPassword()) {
        Serial.println(F("[Hardware] AS608 Fingerprint Sensor Detected Successfully!"));
        if (sensorFaulted) {
            Serial.println(F("[Hardware] Sensor recovered; clearing persisted fault."));
            sensorFaulted = false;
            sensorFaultReason = "";
            persistSensorFault(false, "");
        }
        updateOledStatus("DTRDesk.com", "AS608 Sensor", "Ready Status: OK");
        triggerFeedback(true, 2); // Double beep hardware startup
    } else {
        Serial.println(F("[Hardware] ERROR: Fingerprint sensor not found. Check wiring!"));
        sensorFaulted = true;
        sensorFaultReason = "AS608 fingerprint sensor not detected";
        persistSensorFault(true, sensorFaultReason);
        updateOledStatus("DTRDesk.com", "Sensor Error!", "AS608 Disconnected", false, true);
        triggerFeedback(false, 3);
    }

    // Initialize WiFi Manager (Auto-connect or AP mode)
    wifiManager.begin();
    if (wifiManager.isAPMode()) {
        String apSsidLine = "SSID " + wifiManager.getAPSSID();
        String apPassLine = "Pass " + wifiManager.getAPPassword();
        Serial.printf("[WiFi] AP Mode Active - Connect to: %s\n", wifiManager.getAPSSID().c_str());
        Serial.printf("[WiFi] Password: %s\n", wifiManager.getAPPassword().c_str());
        Serial.printf("[WiFi] Setup URL: http://%s\n", wifiManager.getIP().c_str());
        updateOledStatus("WiFi Setup Mode", apSsidLine.c_str(), apPassLine.c_str());
        // Keep the sensor fault visible even through setup, so a broken sensor
        // is not silently masked by the Wi-Fi provisioning screen.
        if (sensorFaulted) {
            updateOledStatus("SENSOR ERROR", apSsidLine.c_str(), apPassLine.c_str(), false, true);
        }
        ledWrite(LED_RED_PIN, true);
    } else {
        ledWrite(LED_RED_PIN, false);
        updateOledStatus("DTRDesk.com", "WiFi Connected!", wifiManager.getIP().c_str());
        delay(1000);
    }

    // Real wall-clock time is required for both TLS date validation and
    // attendance timestamps; sync via NTP as soon as Wi-Fi is up.
    if (!wifiManager.isAPMode()) {
        syncClockNtp();
    }

    // A gateway flashed from the org portal arrives with either staged org login
    // (primary) or a claim code; exchange it for credentials once Wi-Fi is up.
    if (!wifiManager.isAPMode() &&
        (DeviceConfig::hasPendingRegistration() || DeviceConfig::pendingClaim().length() > 0)) {
        if (!tryRegisterGateway()) tryClaimGateway();
    }

    updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
    printDiagnostics();
}

void loop() {
    // Handle captive portal requests in AP mode
    wifiManager.handleClient();
    unsigned long currentMillis = millis();

    // Serial self-test: type `selftest` (or `diag`) and press Enter.
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();
        if (cmd == "selftest" || cmd == "diag") printDiagnostics();
    }

    // 0. A revoked/rotated key is permanent: stop all cloud work and retrying,
    //    surface it on the OLED and reopen the setup portal for re-provisioning.
    if (deviceRevoked) {
        handleRevocation();
        return;
    }

    // 1. Maintain Wi-Fi Connection
    if (!wifiManager.isAPMode() && WiFi.status() != WL_CONNECTED && currentMillis % 10000 < 50) {
        Serial.println(F("[WiFi] Reconnecting to network..."));
        WiFi.reconnect();
    }

    // 2. Scan Fingerprint Sensor (Non-blocking check). Skipped while locked
    //    down remotely or while the AS608 is faulted.
    if (!isLockdownActive && !sensorFaulted) {
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

                bool clockReady = clockValid();
                unsigned long timestamp = clockReady ? (unsigned long)time(nullptr) : 0UL;
                bool transmitted = clockReady && transmitBiometricLog(slotId, timestamp, false);

                if (transmitted) {
                    Serial.println(F("[Sync] Real-time log transmitted to Cloud API successfully."));
                    updateOledStatus("ACCESS GRANTED", "Clock Recorded!", "Sync: Direct Edge", true);
                    triggerFeedback(true, 1); // Single positive beep
                } else if (deviceRevoked) {
                    // Permanent auth failure: do not buffer a key we can no longer use.
                    Serial.println(F("[Sync] Device revoked; log not buffered."));
                    updateOledStatus("REVOKED", "Key rejected", "Re-provision", false, true);
                    triggerFeedback(false, 2);
                } else {
                    if (clockReady) {
                        Serial.println(F("[Sync] Server unreachable. Buffering log to LittleFS flash memory."));
                        updateOledStatus("OFFLINE LOGGED", "Saved to Flash", "Sync Pending...", true);
                    } else {
                        Serial.println(F("[Sync] Clock not synced yet. Holding log until time sync."));
                        updateOledStatus("OFFLINE LOGGED", "Clock syncing...", "Held in Flash", true);
                    }
                    LittleFSBuffer::appendLog(slotId, timestamp);
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
            if (!clockValid()) {
                // Do not replay records until we can stamp/keep a real epoch.
                Serial.println(F("[LittleFS] Clock not synced yet; deferring offline flush."));
            } else {
                Serial.println(F("\n[LittleFS] Pending offline logs detected. Starting auto-flush..."));
                // Per-record retain: sent records are dropped, only failures
                // stay in the buffer for the next cycle. Back-date records that
                // were captured before the clock synced.
                LittleFSBuffer::flushBuffer([](const LittleFSBuffer::OfflineLog& logItem) -> bool {
                    if (deviceRevoked) return false; // stop retrying on a revoked key
                    unsigned long ts = logItem.timestamp;
                    if (ts < MIN_VALID_EPOCH) {
                        unsigned long nowEpoch = (unsigned long)time(nullptr);
                        unsigned long nowMs = millis();
                        if (nowMs >= logItem.createdAtMs) {
                            unsigned long ageSec = (nowMs - logItem.createdAtMs) / 1000;
                            ts = (nowEpoch > ageSec) ? (nowEpoch - ageSec) : nowEpoch;
                        } else {
                            ts = nowEpoch; // survived a reboot: best available epoch
                        }
                    }
                    return transmitBiometricLog(logItem.slotId, ts, true);
                });

                if (deviceRevoked) {
                    handleRevocation();
                } else if (!LittleFSBuffer::hasPendingLogs()) {
                    Serial.println(F("[LittleFS] All buffered logs successfully synchronized with Cloud!"));
                    updateOledStatus("BUFFER FLUSHED", "All Offline Logs", "Synced to Edge", true);
                    delay(1000);
                    updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
                }
            }
        }
    }

    // A rejected key is permanent; stop the rest of this cycle's cloud work
    // (heartbeat/claim) and re-provision.
    if (deviceRevoked) {
        handleRevocation();
        return;
    }

    // 4. Device Telemetry Heartbeat (Every 30s)
    if (currentMillis - lastHeartbeatTime > HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatTime = currentMillis;
        checkHeartbeat();
    }

    // 5. Retry a pending gateway claim while one is staged (Every 30s)
    if (!wifiManager.isAPMode() &&
        (DeviceConfig::hasPendingRegistration() || DeviceConfig::pendingClaim().length() > 0) &&
        WiFi.status() == WL_CONNECTED && currentMillis - lastClaimTime > 30000) {
        lastClaimTime = currentMillis;
        if (!tryRegisterGateway()) tryClaimGateway();
    }
}

/**
 * Render the status screen. The OLED (128x64, text size 1) and the 2.8" TFT
 * (320x240 landscape, large centered type) share this one entry point.
 */
void updateOledStatus(const char* title, const char* msg1, const char* msg2, bool isSuccess, bool force) {
    if (!isOledConnected) return;
    // A sensor fault is sticky: routine READY/Wi-Fi screens must not hide it.
    // The fault screen itself (and admin states) pass force=true to draw.
    if (sensorFaulted && !force) return;

#ifdef DTRDESK_DISPLAY_ST7789
    const int W = display.width();
    const int H = display.height();

    display.fillScreen(TFT_BLACK);

    // Header band (green = ok, maroon = error/attention).
    const uint16_t headerColor = isSuccess ? TFT_DARKGREEN : TFT_MAROON;
    display.fillRect(0, 0, W, 48, headerColor);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(TFT_WHITE, headerColor);
    display.setTextSize(2);
    display.setCursor(14, 15);
    display.print(title);

    // Primary status line — the big one a user reads from a distance.
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(3);
    display.setCursor(14, 84);
    display.print(msg1);

    // Optional secondary line.
    if (msg2 && strlen(msg2) > 0) {
        display.setTextSize(2);
        display.setCursor(14, 136);
        display.print(msg2);
    }

    // Footer: connectivity + device identity.
    display.fillRect(0, H - 44, W, 44, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.setTextSize(2);
    display.setCursor(14, H - 32);
    if (WiFi.status() == WL_CONNECTED) {
        display.print(clockValid() ? F("WIFI OK | SYNCED") : F("WIFI OK | NTP WAIT"));
    } else {
        display.print(F("OFFLINE | BUFFER"));
    }
#else
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
        display.print(DeviceConfig::hasUsableKey() ? F("WIFI OK | REGISTERED") : F("WIFI OK | UNCLAIMED"));
    } else {
        display.print(F("OFFLINE | FLASH BUFFER"));
    }

    display.display();
#endif
}

/**
 * WiFi is now managed by WiFiManager (captive portal)
 */

/**
 * Persist/restore the AS608 fault so telemetry keeps reporting it across reboots.
 */
void persistSensorFault(bool faulted, const String& reason) {
    File file = LittleFS.open(SENSOR_FAULT_FILE, "w");
    if (!file) return;
    StaticJsonDocument<256> doc;
    doc["fault"] = faulted;
    doc["reason"] = reason;
    serializeJson(doc, file);
    file.close();
}

void loadSensorFault() {
    if (!LittleFS.exists(SENSOR_FAULT_FILE)) return;
    File file = LittleFS.open(SENSOR_FAULT_FILE, "r");
    if (!file) return;
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) return;
    sensorFaulted = doc["fault"] | false;
    sensorFaultReason = (const char*)(doc["reason"] | "");
    if (sensorFaulted) {
        Serial.printf("[Hardware] Persisted sensor fault: %s\n", sensorFaultReason.c_str());
    }
}

/**
 * A 401/403 from the API is permanent (revoked key / rotated key). Stop cloud
 * work, show it clearly, and reopen the captive portal for re-provisioning.
 */
void handleRevocation() {
    if (revocationHandled) return;
    revocationHandled = true;
    Serial.println(F("[Auth] Device key rejected (401/403). Re-provision required."));
    triggerFeedback(false, 3);
    wifiManager.openSetupPortal();
    // Show how to reach the portal so the device can actually be re-provisioned.
    String apSsidLine = "SSID " + wifiManager.getAPSSID();
    String apPassLine = "Pass " + wifiManager.getAPPassword();
    updateOledStatus("REVOKED", apSsidLine.c_str(), apPassLine.c_str(), false, true);
}

/**
 * Hardware Feedback (LED & Buzzer Indicator)
 */
void triggerFeedback(bool success, int beepCount) {
    int targetPin = success ? LED_GREEN_PIN : LED_RED_PIN;

    for (int i = 0; i < beepCount; i++) {
        ledWrite(targetPin, true);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(80);
        ledWrite(targetPin, false);
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
 * Capture one enrollment image into a buffer with up to 3 quality attempts,
 * prompting on the OLED between tries. Returns true on a good capture.
 */
static bool captureEnrollTemplate(uint8_t slot, const char* prompt, const char* fingerLabel) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        char line[24];
        snprintf(line, sizeof(line), "%s (%d/3)", prompt, attempt);
        updateOledStatus("ENROLL MODE", line, fingerLabel);
        triggerFeedback(attempt == 1, 1);

        unsigned long start = millis();
        int p = FINGERPRINT_NOFINGER;
        while (millis() - start < 12000) { // wait up to 12s for a finger
            p = finger.getImage();
            if (p == FINGERPRINT_OK) break;
            delay(50);
        }
        if (p != FINGERPRINT_OK) continue;              // timed out -> retry
        if (finger.image2Tz(slot) == FINGERPRINT_OK) return true; // poor image -> retry
    }
    return false;
}

/**
 * Interactive biometric enrollment with up to 3 full attempts
 * (place -> remove -> place again -> store), showing the finger being
 * enrolled and the attempt count on the OLED.
 */
bool enrollFingerprint(int slotId, const char* fingerName) {
    const char* label = (fingerName && strlen(fingerName) > 0) ? fingerName : "Finger";
    Serial.printf("[Enroll] slot #%d finger=%s\n", slotId, label);

    for (int round = 1; round <= 3; round++) {
        if (!captureEnrollTemplate(1, "Place finger", label)) {
            updateOledStatus("ENROLL MODE", "No finger read", "Try again...");
            continue;
        }

        // Lift the finger before the second capture.
        updateOledStatus("ENROLL MODE", "Remove finger", label);
        delay(500);
        unsigned long liftStart = millis();
        while (finger.getImage() != FINGERPRINT_NOFINGER && millis() - liftStart < 5000) delay(50);

        if (!captureEnrollTemplate(2, "Place again", label)) continue;

        if (finger.createModel() != FINGERPRINT_OK) {
            updateOledStatus("ENROLL MODE", "Prints differ", "Retry...");
            triggerFeedback(false, 2);
            continue;
        }
        if (finger.storeModel(slotId) == FINGERPRINT_OK) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Slot #%d saved!", slotId);
            updateOledStatus("ENROLLED", buf, label, true);
            triggerFeedback(true, 2);
            delay(1800);
            updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
            return true;
        }
        updateOledStatus("ENROLL ERROR", "Storage failed", "Memory full", false);
        triggerFeedback(false, 2);
        return false;
    }

    updateOledStatus("ENROLL ERROR", "3 attempts failed", "Try again", false);
    triggerFeedback(false, 3);
    delay(1500);
    updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
    return false;
}

/**
 * PRIMARY onboarding: register this gateway with the organization credentials
 * entered in the captive portal (POST /api/v1/devices/register). On success the
 * device receives its own API key and is bound to that org.
 */
bool tryRegisterGateway() {
    static int regFailures = 0;
    if (!DeviceConfig::hasPendingRegistration() || WiFi.status() != WL_CONNECTED) return false;

    Serial.printf("[Register] Registering gateway with org %s...\n", DeviceConfig::regOrg().c_str());
    updateOledStatus("REGISTERING", "Linking gateway", "to your org...");

    WiFiClientSecure client;
    configureTls(client);
    HTTPClient http;
    http.begin(client, DTRDESK_REGISTER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    StaticJsonDocument<384> req;
    req["org_code"] = DeviceConfig::regOrg();
    req["email"] = DeviceConfig::regEmail();
    req["password"] = DeviceConfig::regPassword();
    String location = DeviceConfig::regLocation();
    if (location.length() > 0) req["location_name"] = location;
    String body;
    serializeJson(req, body);

    int httpCode = http.POST(body);
    if (httpCode == 200 || httpCode == 201) {
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, http.getString());
        JsonObject p = doc["provisioning"];
        String id = p["DTRDESK_DEVICE_ID"] | "";
        String key = p["DTRDESK_DEVICE_KEY"] | "";
        String api = p["DTRDESK_API_URL"] | "";
        String sync = p["DTRDESK_SYNC_URL"] | "";
        http.end();
        if (!err && id.length() > 0 && key.length() > 0) {
            DeviceConfig::save(id, key, api, sync); // also clears the staged login
            DeviceConfig::clearRegistration();
            regFailures = 0;
            Serial.printf("[Register] Gateway linked as %s\n", id.c_str());
            updateOledStatus("REGISTERED", "Gateway linked", "Ready for scans", true);
            triggerFeedback(true, 2);
            delay(1500);
            return true;
        }
        Serial.println(F("[Register] Malformed response; will retry."));
    } else {
        Serial.printf("[Register] HTTP %d; will retry.\n", httpCode);
        http.end();
        updateOledStatus("REGISTER FAILED", "Check org login", "Retrying...", false);
        delay(1200);
    }

    if (++regFailures >= 10) {
        regFailures = 0;
        Serial.println(F("[Register] Too many failures; reopening setup portal."));
        updateOledStatus("SETUP MODE", "Registration failed", "Connect: DTRDesk-Setup", false);
        delay(1800);
        wifiManager.openSetupPortal();
    }
    return false;
}

/**
 * Exchange a staged Claim Code for this gateway's own credentials
 * (POST /api/v1/devices/claim). Runs over HTTPS after Wi-Fi is up.
 */
bool tryClaimGateway() {
    static int claimFailures = 0;
    String code = DeviceConfig::pendingClaim();
    if (code.length() == 0 || WiFi.status() != WL_CONNECTED) return false;

    Serial.printf("[Claim] Exchanging claim code %s...\n", code.c_str());
    updateOledStatus("CLAIMING", "Linking gateway", "to your org...");

    WiFiClientSecure client;
    configureTls(client);
    HTTPClient http;
    http.begin(client, DTRDESK_CLAIM_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    StaticJsonDocument<128> req;
    req["claim_code"] = code;
    // The server binds the gateway the claim code belongs to and returns 409 if
    // a supplied device_id differs, so never send the compile-time placeholder.
    // Only a runtime-provisioned id (an already-claimed device) is trustworthy.
    if (DeviceConfig::isProvisioned()) req["device_id"] = DeviceConfig::deviceId();
    String body;
    serializeJson(req, body);

    int httpCode = http.POST(body);
    if (httpCode == 200) {
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, http.getString());
        JsonObject p = doc["provisioning"];
        String id = p["DTRDESK_DEVICE_ID"] | "";
        String key = p["DTRDESK_DEVICE_KEY"] | "";
        String api = p["DTRDESK_API_URL"] | "";
        String sync = p["DTRDESK_SYNC_URL"] | "";
        http.end();
        if (!err && id.length() > 0 && key.length() > 0) {
            DeviceConfig::save(id, key, api, sync); // persists without claim_code
            DeviceConfig::clearPendingClaim();
            claimFailures = 0;
            Serial.printf("[Claim] Gateway linked as %s\n", id.c_str());
            updateOledStatus("CLAIMED", "Gateway linked", "Ready for scans", true);
            triggerFeedback(true, 2);
            delay(1500);
            return true;
        }
        Serial.println(F("[Claim] Malformed response; will retry."));
    } else {
        Serial.printf("[Claim] HTTP %d; will retry.\n", httpCode);
        http.end();
        updateOledStatus("CLAIM FAILED", "Check code & WiFi", "Retrying...", false);
        delay(1200);
    }

    // After repeated failures (likely a wrong/expired code) re-open the setup
    // portal so the user can enter a fresh claim code.
    if (++claimFailures >= 10) {
        claimFailures = 0;
        Serial.println(F("[Claim] Too many failures; reopening setup portal."));
        updateOledStatus("SETUP MODE", "Claim failed", "Connect: DTRDesk-Setup", false);
        delay(1800);
        wifiManager.openSetupPortal();
    }
    return false;
}

/**
 * Post Biometric Scan Payload to Cloud API (POST /api/v1/device/logs)
 */
bool transmitBiometricLog(int slotId, unsigned long timestamp, bool isOfflineBuff) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Never submit a placeholder/pre-sync timestamp to the attendance log.
    if (timestamp < MIN_VALID_EPOCH || !clockValid()) return false;

    WiFiClientSecure client;
    configureTls(client);
    HTTPClient http;

    String apiUrl = DeviceConfig::apiUrl();
    String deviceId = DeviceConfig::deviceId();
    String deviceKey = DeviceConfig::deviceKey();

    http.begin(client, apiUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-DTRDesk-Device-Key", deviceKey);
    http.setTimeout(4000);

    StaticJsonDocument<256> doc;
    doc["device_id"] = deviceId;
    doc["fingerprint_slot_id"] = slotId;
    doc["raw_timestamp"] = timestamp;
    doc["is_offline_buff"] = isOfflineBuff;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload);
    bool success = (httpCode == 200 || httpCode == 201);

    if (httpCode == 401 || httpCode == 403) {
        // Permanent: the device key was revoked or rotated.
        Serial.printf("[API Error] HTTP %d - device key rejected (revoked/rotated).\n", httpCode);
        deviceRevoked = true;
    } else if (success) {
        String response = http.getString();
        Serial.printf("[API Code %d] Response: %s\n", httpCode, response.c_str());
    } else {
        Serial.printf("[API Error] HTTP POST failed with code: %d\n", httpCode);
    }

    http.end();
    return success;
}

/**
 * Report the outcome of a remote enrollment so the server records the
 * fingerprint only after a real, on-device success.
 */
void reportEnrollmentResult(int slotId, bool success, const char* finger) {
    if (WiFi.status() != WL_CONNECTED) return;

    String url = String(DTRDESK_ENROLL_RESULT_BASE) + "/" + DeviceConfig::deviceId() + "/enroll/result";
    WiFiClientSecure client;
    configureTls(client);
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-DTRDesk-Device-Key", DeviceConfig::deviceKey());
    http.setTimeout(8000);

    StaticJsonDocument<160> doc;
    doc["slot_id"] = slotId;
    doc["success"] = success;
    if (finger && strlen(finger) > 0) doc["finger"] = finger;
    String body;
    serializeJson(doc, body);

    int httpCode = http.POST(body);
    if (httpCode == 401 || httpCode == 403) {
        Serial.printf("[Enroll] HTTP %d - device key rejected (revoked/rotated).\n", httpCode);
        deviceRevoked = true;
    }
    Serial.printf("[Enroll] Result reported (%s): HTTP %d\n", success ? "ok" : "failed", httpCode);
    http.end();
}

/**
 * Send Sync Heartbeat & Check Remote Commands from Cloud (Phase 3)
 */
void checkHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return;
    lastHeartbeatAttempted = true;
    lastHeartbeatOk = false;

    WiFiClientSecure client;
    configureTls(client);
    HTTPClient http;

    String syncUrl = DeviceConfig::syncUrl();
    String deviceId = DeviceConfig::deviceId();
    String deviceKey = DeviceConfig::deviceKey();

    http.begin(client, syncUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-DTRDesk-Device-Key", deviceKey);
    http.setTimeout(3000);

    StaticJsonDocument<384> doc;
    doc["device_id"] = deviceId;
    doc["firmware_version"] = DeviceConfig::firmwareVersion();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    // Real pending count (not a boolean) plus an explicit overflow flag.
    doc["flash_buffer_count"] = LittleFSBuffer::count();
    doc["flash_buffer_full"] = LittleFSBuffer::isFull();
    if (sensorFaulted) doc["last_error"] = sensorFaultReason;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload);
    if (httpCode == 401 || httpCode == 403) {
        // Permanent: revoked/rotated key. Stop everything and re-provision.
        Serial.printf("[Heartbeat] HTTP %d - device key rejected (revoked/rotated).\n", httpCode);
        deviceRevoked = true;
    } else if (httpCode == 200) {
        lastHeartbeatOk = true;
        String resp = http.getString();
        StaticJsonDocument<384> respDoc;
        DeserializationError err = deserializeJson(respDoc, resp);
        if (!err) {
            // NTP unavailable? Adopt the server's epoch from the heartbeat.
            long long serverTs = respDoc["server_timestamp"] | 0LL;
            if (!clockValid() && serverTs >= (long long)MIN_VALID_EPOCH) {
                struct timeval tv;
                tv.tv_sec = (time_t)serverTs;
                tv.tv_usec = 0;
                settimeofday(&tv, nullptr);
                Serial.printf("[NTP] Adopted server epoch %lld\n", serverTs);
            }
            const char* mode = respDoc["mode"] | "NORMAL";
            // Remote LOCKDOWN: the backend command channel is the `mode` field
            // (same channel as ENROLL). Also accept an explicit boolean flag.
            bool lockdown = (strcmp(mode, "LOCKDOWN") == 0);
            if (!lockdown && respDoc.containsKey("lockdown")) {
                lockdown = respDoc["lockdown"].as<bool>();
            }
            if (lockdown != isLockdownActive) {
                isLockdownActive = lockdown;
                if (lockdown) {
                    Serial.println(F("[Command] Remote LOCKDOWN active. Scanning halted."));
                    updateOledStatus("LOCKED", "Remote lockdown", "Scanning halted", false, true);
                } else {
                    Serial.println(F("[Command] Lockdown cleared. Resuming scans."));
                    updateOledStatus("DTRDesk.com", "READY FOR SCAN", "Place Finger...");
                }
            }
            if (strcmp(mode, "ENROLL") == 0 && !isLockdownActive && !sensorFaulted) {
                int targetSlot = respDoc["enroll_target_slot"] | 1;
                String fingerLabel = respDoc["enroll_finger"] | "";
                fingerLabel.replace("_", " ");
                bool enrolled = enrollFingerprint(targetSlot, fingerLabel.c_str());
                reportEnrollmentResult(targetSlot, enrolled, fingerLabel.c_str());
            }
        }
        Serial.println(F("[Heartbeat] Telemetry sync OK."));
    }
    http.end();
}

/**
 * Serial self-test snapshot (type `selftest` + Enter on the 115200 console).
 * Prints one line per subsystem so a hardware failure is unambiguous; it makes
 * no cloud calls. Used by HARDWARE_TEST.md.
 */
void printDiagnostics() {
    Serial.println(F("\n---- DTRDesk self-test ----"));
    Serial.printf("[diag] firmware      : %s\n", DeviceConfig::firmwareVersion().c_str());
    Serial.printf("[diag] device_id     : %s\n", DeviceConfig::deviceId().c_str());
    Serial.printf("[diag] provisioned   : %s (usable key: %s)\n",
                  DeviceConfig::isProvisioned() ? "yes" : "no",
                  DeviceConfig::hasUsableKey() ? "yes" : "no");
    Serial.printf("[diag] wifi          : %s  rssi=%d dBm  ip=%s\n",
                  WiFi.status() == WL_CONNECTED ? "connected" : "offline",
                  WiFi.RSSI(), WiFi.localIP().toString().c_str());
    Serial.printf("[diag] clock         : %s (%lu)\n",
                  clockValid() ? "synced" : "NOT synced", (unsigned long)time(nullptr));
    Serial.printf("[diag] cloud         : %s\n",
                  !lastHeartbeatAttempted ? "no heartbeat yet"
                                          : (lastHeartbeatOk ? "heartbeat ok" : "heartbeat FAILED"));
    Serial.printf("[diag] sensor (AS608): %s%s%s\n", sensorFaulted ? "FAULT" : "ok",
                  (sensorFaulted && sensorFaultReason.length()) ? " - " : "",
                  sensorFaulted ? sensorFaultReason.c_str() : "");
    Serial.printf("[diag] oled (SSD1306): %s\n", isOledConnected ? "ok" : "NOT detected");
    Serial.printf("[diag] offline buffer: %d record(s), %u bytes, full=%s\n",
                  LittleFSBuffer::count(), (unsigned)LittleFSBuffer::bufferBytes(),
                  LittleFSBuffer::isFull() ? "yes" : "no");
    Serial.printf("[diag] lockdown      : %s\n", isLockdownActive ? "ACTIVE" : "off");
    Serial.printf("[diag] revoked       : %s\n", deviceRevoked ? "YES - re-provision" : "no");
    Serial.println(F("---------------------------\n"));
}
