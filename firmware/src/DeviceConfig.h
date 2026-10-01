#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "config.h" // compile-time defaults from build_config.h

// ============================================================================
// DTRDesk Device Credentials
//
// Device identity (ID, one-time API key, API/sync URLs) can be provisioned at
// runtime through the captive portal and stored in LittleFS, instead of being
// baked into the firmware at compile time. This lets one generic binary be
// flashed to many devices, each claiming its own key.
//
// Precedence: runtime /device_config.json  >  compile-time build_config.h.
// Devices flashed over serial with a real .env keep working unchanged.
// ============================================================================
class DeviceConfig {
public:
    // Load runtime overrides (call after LittleFS is mounted).
    static void begin() {
        load();
    }

    static void load() {
        // Start from compile-time defaults, then overlay any runtime file.
        _deviceId() = String(DTRDESK_DEVICE_ID);
        _deviceKey() = String(DTRDESK_DEVICE_KEY);
        _apiUrl() = String(DTRDESK_API_URL);
        _syncUrl() = String(DTRDESK_SYNC_URL);
        _hasRuntime() = false;
        _claimCode() = "";
        _regOrg() = "";
        _regEmail() = "";
        _regPassword() = "";
        _regLocation() = "";

        File file = LittleFS.open("/device_config.json", "r");
        if (!file) return;

        StaticJsonDocument<1024> doc;
        DeserializationError err = deserializeJson(doc, file);
        file.close();
        if (err) return;

        const char* id = doc["device_id"] | "";
        const char* key = doc["device_key"] | "";
        const char* api = doc["api_url"] | "";
        const char* sync = doc["sync_url"] | "";
        const char* claim = doc["claim_code"] | "";
        const char* ro = doc["reg_org"] | "";
        const char* re = doc["reg_email"] | "";
        const char* rp = doc["reg_password"] | "";
        const char* rl = doc["reg_location"] | "";

        if (strlen(id) > 0) _deviceId() = String(id);
        if (strlen(key) > 0) _deviceKey() = String(key);
        if (strlen(api) > 0) _apiUrl() = String(api);
        if (strlen(sync) > 0) _syncUrl() = String(sync);
        if (strlen(claim) > 0) _claimCode() = String(claim);
        if (strlen(ro) > 0) _regOrg() = String(ro);
        if (strlen(re) > 0) _regEmail() = String(re);
        if (strlen(rp) > 0) _regPassword() = String(rp);
        if (strlen(rl) > 0) _regLocation() = String(rl);

        _hasRuntime() = strlen(id) > 0 && strlen(key) > 0;
    }

    static bool save(const String& deviceId, const String& deviceKey,
                     const String& apiUrl, const String& syncUrl) {
        StaticJsonDocument<512> doc;
        doc["device_id"] = deviceId;
        doc["device_key"] = deviceKey;
        if (apiUrl.length() > 0) doc["api_url"] = apiUrl;
        if (syncUrl.length() > 0) doc["sync_url"] = syncUrl;

        File file = LittleFS.open("/device_config.json", "w");
        if (!file) return false;
        serializeJson(doc, file);
        file.close();
        load();
        return _hasRuntime();
    }

    static void clear() {
        LittleFS.remove("/device_config.json");
        load();
    }

    // Persist a claim code entered in the captive portal. The firmware exchanges
    // it for real credentials once Wi-Fi is up (see main.cpp tryClaimGateway()).
    static bool saveClaimCode(const String& claimCode) {
        StaticJsonDocument<256> doc;
        if (_hasRuntime()) { // keep existing credentials if already provisioned
            doc["device_id"] = _deviceId();
            doc["device_key"] = _deviceKey();
            if (_apiUrl().length()) doc["api_url"] = _apiUrl();
            if (_syncUrl().length()) doc["sync_url"] = _syncUrl();
        }
        doc["claim_code"] = claimCode;
        File file = LittleFS.open("/device_config.json", "w");
        if (!file) return false;
        serializeJson(doc, file);
        file.close();
        load();
        return true;
    }

    static String pendingClaim() { return _claimCode(); }

    static void clearPendingClaim() { _claimCode() = ""; }

    // PRIMARY onboarding: stage the org credentials entered in the captive
    // portal. The firmware registers the gateway itself once Wi-Fi is up
    // (see main.cpp tryRegisterGateway()), binding it to that organization.
    static bool saveRegistration(const String& org, const String& email,
                                 const String& password, const String& location) {
        StaticJsonDocument<1024> doc;
        if (_hasRuntime()) { // keep existing credentials if already provisioned
            doc["device_id"] = _deviceId();
            doc["device_key"] = _deviceKey();
            if (_apiUrl().length()) doc["api_url"] = _apiUrl();
            if (_syncUrl().length()) doc["sync_url"] = _syncUrl();
        }
        doc["reg_org"] = org;
        doc["reg_email"] = email;
        doc["reg_password"] = password;
        if (location.length()) doc["reg_location"] = location;
        File file = LittleFS.open("/device_config.json", "w");
        if (!file) return false;
        serializeJson(doc, file);
        file.close();
        load();
        return true;
    }

    static bool hasPendingRegistration() {
        return _regOrg().length() > 0 && _regEmail().length() > 0 && _regPassword().length() > 0;
    }
    static String regOrg()      { return _regOrg(); }
    static String regEmail()    { return _regEmail(); }
    static String regPassword() { return _regPassword(); }
    static String regLocation() { return _regLocation(); }

    static void clearRegistration() {
        _regOrg() = ""; _regEmail() = ""; _regPassword() = ""; _regLocation() = "";
    }

    // True when a runtime credential file exists (i.e. this device was claimed).
    static bool isProvisioned() { return _hasRuntime(); }

    static String deviceId()  { return _deviceId(); }
    static String deviceKey() { return _deviceKey(); }
    static String apiUrl()    { return _apiUrl().length()  ? _apiUrl()  : String(DTRDESK_API_URL); }
    static String syncUrl()   { return _syncUrl().length() ? _syncUrl() : String(DTRDESK_SYNC_URL); }
    static String firmwareVersion() { return String(DTRDESK_FIRMWARE_VERSION); }

    // Reject obvious placeholders so an unclaimed device is flagged in the UI.
    static bool hasUsableKey() {
        String k = deviceKey();
        return k.length() > 0 && k.indexOf("replace-with") == -1 && k != "password";
    }

private:
    // Function-local statics avoid out-of-line definitions (header-only module).
    static String& _deviceId()  { static String v; return v; }
    static String& _deviceKey() { static String v; return v; }
    static String& _apiUrl()    { static String v; return v; }
    static String& _syncUrl()   { static String v; return v; }
    static String& _claimCode() { static String v; return v; }
    static String& _regOrg()      { static String v; return v; }
    static String& _regEmail()    { static String v; return v; }
    static String& _regPassword() { static String v; return v; }
    static String& _regLocation() { static String v; return v; }
    static bool&   _hasRuntime(){ static bool v = false; return v; }
};

#endif // DEVICE_CONFIG_H
