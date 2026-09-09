#ifndef LITTLEFS_BUFFER_H
#define LITTLEFS_BUFFER_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>

#define BUFFER_FILE_PATH "/offline_buffer.txt"
#define TEMP_FILE_PATH   "/offline_temp.txt"

class LittleFSBuffer {
public:
    struct OfflineLog {
        int slotId;
        unsigned long timestamp;
    };

    static void begin() {
        if (!LittleFS.begin()) {
            Serial.println(F("[LittleFS] Mounting file system failed! Formatting..."));
            LittleFS.format();
            LittleFS.begin();
        }
        Serial.println(F("[LittleFS] File system mounted successfully."));
    }

    // Save failed log scan line to buffer file
    static bool saveLog(int slotId, unsigned long timestamp) {
        File file = LittleFS.open(BUFFER_FILE_PATH, "a");
        if (!file) {
            Serial.println(F("[LittleFS] Error opening buffer file for append!"));
            return false;
        }

        StaticJsonDocument<256> doc;
        doc["slot_id"] = slotId;
        doc["raw_ts"] = timestamp;
        doc["created"] = millis();

        serializeJson(doc, file);
        file.println();
        file.close();

        Serial.printf("[LittleFS] Saved offline log for Fingerprint Slot #%d to Flash buffer.\n", slotId);
        return true;
    }

    static bool appendLog(int slotId, unsigned long timestamp) {
        return saveLog(slotId, timestamp);
    }

    static bool hasPendingLogs() {
        return getPendingCount() > 0;
    }

    static std::vector<OfflineLog> getPendingLogs() {
        std::vector<OfflineLog> pendingLogs;
        if (!LittleFS.exists(BUFFER_FILE_PATH)) return pendingLogs;

        File file = LittleFS.open(BUFFER_FILE_PATH, "r");
        if (!file) return pendingLogs;

        while (file.available()) {
            String line = file.readStringUntil('\n');
            line.trim();
            if (line.length() < 5) continue;

            StaticJsonDocument<256> doc;
            if (deserializeJson(doc, line)) continue;

            OfflineLog logItem;
            logItem.slotId = doc["slot_id"] | -1;
            logItem.timestamp = doc["raw_ts"] | 0UL;
            if (logItem.slotId > 0) pendingLogs.push_back(logItem);
        }

        file.close();
        return pendingLogs;
    }

    static void clearBuffer() {
        LittleFS.remove(BUFFER_FILE_PATH);
    }

    // Get total count of buffered offline records
    static int getPendingCount() {
        if (!LittleFS.exists(BUFFER_FILE_PATH)) return 0;
        File file = LittleFS.open(BUFFER_FILE_PATH, "r");
        if (!file) return 0;

        int count = 0;
        while (file.available()) {
            String line = file.readStringUntil('\n');
            if (line.length() > 5) count++;
        }
        file.close();
        return count;
    }

    // Flush offline buffer line by line via callback handler
    typedef std::function<bool(int slotId, unsigned long timestamp)> FlushCallback;

    static void flushBuffer(FlushCallback sendFunc) {
        if (!LittleFS.exists(BUFFER_FILE_PATH)) return;

        File readFile = LittleFS.open(BUFFER_FILE_PATH, "r");
        if (!readFile || !readFile.available()) {
            if (readFile) readFile.close();
            LittleFS.remove(BUFFER_FILE_PATH);
            return;
        }

        File tempFile = LittleFS.open(TEMP_FILE_PATH, "w");
        if (!tempFile) {
            Serial.println(F("[LittleFS] Error creating temp file for buffer flush!"));
            readFile.close();
            return;
        }

        int successCount = 0;
        int failCount = 0;

        while (readFile.available()) {
            String line = readFile.readStringUntil('\n');
            line.trim();
            if (line.length() < 5) continue;

            StaticJsonDocument<256> doc;
            DeserializationError err = deserializeJson(doc, line);
            if (err) continue;

            int slotId = doc["slot_id"];
            unsigned long rawTs = doc["raw_ts"];

            // Attempt cloud push
            bool sent = sendFunc(slotId, rawTs);

            if (sent) {
                successCount++;
                Serial.printf("[LittleFS] Successfully flushed offline slot #%d to Cloud.\n", slotId);
            } else {
                // Keep failed record in temp file to retry later
                tempFile.println(line);
                failCount++;
                Serial.printf("[LittleFS] Flush failed for slot #%d. Keeping in buffer.\n", slotId);
            }
        }

        readFile.close();
        tempFile.close();

        // Replace buffer with remaining unsent logs
        LittleFS.remove(BUFFER_FILE_PATH);
        if (failCount > 0) {
            LittleFS.rename(TEMP_FILE_PATH, BUFFER_FILE_PATH);
        } else {
            LittleFS.remove(TEMP_FILE_PATH);
        }

        Serial.printf("[LittleFS] Flush complete. Synced: %d, Remaining in buffer: %d\n", successCount, failCount);
    }
};

#endif // LITTLEFS_BUFFER_H
