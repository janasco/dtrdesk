#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "DeviceConfig.h"

// ============================================================================
// DTRDesk WiFi Manager - Captive Portal for WiFi Provisioning
// ============================================================================

class DTRDeskWiFiManager {
private:
    ESP8266WebServer server;
    DNSServer dnsServer; // hijacks all DNS so phones auto-open the portal
    bool dnsStarted = false;
    String apSSID;
    String apPassword;
    String savedSSID;
    String savedPassword;
    bool apMode = false;
    bool serverConfigured = false; // routes registered once

    // Captive portal HTML page
    const char* portalHTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>DTRDesk WiFi Setup</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        html, body { overflow-x: hidden; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #0b0f16;
            color: #e5e7eb;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: flex-start;
            padding: 16px;
        }
        .container {
            background: #111827;
            border: 1px solid #1f2937;
            border-radius: 16px;
            padding: clamp(20px, 5vw, 32px);
            max-width: 460px;
            width: 100%;
            margin: auto;
        }
        .logo { text-align: center; margin-bottom: 20px; }
        .logo h1 { font-size: 24px; color: #fff; letter-spacing: .02em; }
        .logo p { color: #9ca3af; margin-top: 4px; font-size: 14px; }
        .form-group { margin-bottom: 16px; }
        label { display: block; margin-bottom: 6px; font-weight: 600; color: #cbd5e1; font-size: 14px; }
        input, select {
            width: 100%;
            min-width: 0;
            padding: 13px 14px;
            border: 1px solid #334155;
            border-radius: 10px;
            background: #0b1220;
            color: #fff;
            font-size: 16px;
        }
        input:focus, select:focus { outline: none; border-color: #38bdf8; }
        /* Long SSIDs must not break the layout. */
        select { text-overflow: ellipsis; white-space: nowrap; }
        .row { display: flex; gap: 8px; align-items: stretch; }
        .row select { flex: 1 1 auto; min-width: 0; }
        .btn-secondary { flex: 0 0 auto; white-space: nowrap; padding: 0 14px; }
        .input-wrap { position: relative; }
        .input-wrap input { padding-right: 64px; }
        .toggle-eye {
            position: absolute; right: 6px; top: 50%; transform: translateY(-50%);
            background: transparent; border: 0; color: #93c5fd; font-size: 12px; font-weight: 700;
            cursor: pointer; padding: 8px; border-radius: 8px;
        }
        .toggle-eye:hover { background: rgba(56,189,248,0.12); }
        .hint { color: #94a3b8; font-size: 12px; margin-top: 6px; line-height: 1.45; }
        .section-title { color: #38bdf8; font-size: 14px; font-weight: 700; margin: 22px 0 8px; }
        .btn, .btn-secondary { border: none; border-radius: 10px; cursor: pointer; font-weight: 700; font-size: 15px; background: #0284c7; color: #fff; }
        .btn { width: 100%; padding: 14px 16px; }
        .btn:hover, .btn-secondary:hover { background: #0369a1; }
        .btn-secondary { background: #1e293b; padding: 0 14px; }
        details summary { cursor: pointer; color: #94a3b8; font-size: 13px; margin: 8px 0 12px; }
        .scan-hint { text-align: center; margin-top: 16px; padding: 12px; border-radius: 8px; background: #0b1220; border: 1px solid #1f2937; font-size: 13px; color: #94a3b8; }
        .status { text-align: center; margin-top: 16px; padding: 12px; border-radius: 8px; background: #0b1220; font-size: 14px; color: #94a3b8; display: none; }
        .status.show { display: block; }
        .status.success { background: rgba(16,185,129,.15); color: #34d399; }
        .status.error { background: rgba(244,63,94,.15); color: #fb7185; }
    </style>
</head>
<body>
    <div class="container">
        <div class="logo">
            <h1>DTRDesk</h1>
            <p>Biometric Access System</p>
        </div>
        <form id="wifiForm" action="/save" method="POST">
            <div class="form-group">
                <label for="ssid">WiFi Network</label>
                <div class="row">
                    <select id="ssid" name="ssid" required>
                        <option value="">Scanning nearby networks…</option>
                    </select>
                    <button type="button" id="rescan" class="btn-secondary">Rescan</button>
                </div>
                <p id="scanmsg" class="hint">Looking for Wi-Fi networks…</p>
                <label style="display:flex;align-items:center;gap:8px;margin-top:8px;color:#94a3b8;font-size:13px;font-weight:500;">
                    <input type="checkbox" id="manualToggle" style="width:auto;"> Hidden network? Enter the name manually
                </label>
                <input type="text" id="ssid_manual" placeholder="Network name (SSID)" autocomplete="off" style="display:none;margin-top:8px;">
            </div>
            <div class="form-group">
                <label for="password">WiFi Password <span style="color:#94a3b8;font-weight:400;">(leave empty for open networks)</span></label>
                <div class="input-wrap">
                    <input type="password" id="password" name="password" placeholder="Wi-Fi password">
                    <button type="button" class="toggle-eye" data-target="password">SHOW</button>
                </div>
            </div>
            <p class="section-title">Link this gateway to your organization</p>
            <div class="form-group">
                <label for="reg_org">Org ID</label>
                <input type="text" id="reg_org" name="reg_org" placeholder="e.g. K7QX2A" autocomplete="off" style="text-transform:uppercase;">
            </div>
            <div class="form-group">
                <label for="reg_email">Organization admin email</label>
                <input type="text" id="reg_email" name="reg_email" placeholder="you@yourcompany.com" autocomplete="off">
                <p class="hint">The account you used to register your organization in your DTRDesk org portal — an admin, not an employee.</p>
            </div>
            <div class="form-group">
                <label for="reg_password">Organization admin password</label>
                <div class="input-wrap">
                    <input type="password" id="reg_password" name="reg_password" placeholder="Your DTRDesk admin password" autocomplete="off">
                    <button type="button" class="toggle-eye" data-target="reg_password">SHOW</button>
                </div>
            </div>
            <div class="form-group">
                <label for="reg_location">Location</label>
                <input type="text" id="reg_location" name="reg_location" placeholder="e.g. Main Entrance" autocomplete="off">
            </div>
            <details>
                <summary style="cursor:pointer;color:#aaa;font-size:13px;margin:4px 0 12px;">Advanced: claim code or manual Device ID + key</summary>
                <div class="form-group">
                    <label for="claim_code">Gateway Claim Code</label>
                    <input type="text" id="claim_code" name="claim_code" placeholder="8-char code from your org portal" autocomplete="off">
                    <p style="color:#888;font-size:12px;margin-top:6px;">Alternative to the org login above: bind the gateway in the org portal, then paste its Claim Code here.</p>
                </div>
                <div class="form-group">
                    <label for="device_id">Device ID</label>
                    <input type="text" id="device_id" name="device_id" placeholder="e.g. ESP8266_GATE_01" value="{{DEVICE_ID}}">
                </div>
                <div class="form-group">
                    <label for="device_key">Device Key (one-time)</label>
                    <input type="text" id="device_key" name="device_key" placeholder="Paste key from the dashboard (blank = keep)">
                </div>
                <div class="form-group">
                    <label for="api_url">API URL</label>
                    <input type="text" id="api_url" name="api_url" value="{{API_URL}}">
                </div>
                <div class="form-group">
                    <label for="sync_url">Sync URL</label>
                    <input type="text" id="sync_url" name="sync_url" value="{{SYNC_URL}}">
                </div>
            </details>
            <button type="submit" class="btn">Save &amp; Connect</button>
        </form>
        <div id="status" class="status"></div>
        <div class="scan-hint">
            <strong>Tip:</strong> Point your finger at the sensor to enroll fingerprints after setup.
        </div>
    </div>
    <script>
        function scanNetworks() {
            var sel = document.getElementById('ssid');
            var msg = document.getElementById('scanmsg');
            msg.textContent = 'Scanning nearby networks…';
            var x = new XMLHttpRequest();
            x.open('GET', '/scan', true);
            x.onreadystatechange = function() {
                if (x.readyState !== 4) return;
                if (x.status !== 200) { msg.textContent = 'Scan failed (' + x.status + '). Tap Rescan.'; return; }
                try {
                    var nets = (JSON.parse(x.responseText).networks) || [];
                    sel.innerHTML = '';
                    if (!nets.length) {
                        msg.textContent = 'No networks found. Tap Rescan.';
                        return;
                    }
                    for (var i = 0; i < nets.length; i++) {
                        var n = nets[i];
                        var o = document.createElement('option');
                        o.value = n.ssid; // full SSID is what gets saved
                        var label = n.ssid.length > 24 ? n.ssid.slice(0, 23) + '…' : n.ssid;
                        o.textContent = label + '  (' + n.rssi + ' dBm · ' + (n.secure ? 'secured' : 'open') + ')';
                        sel.appendChild(o);
                    }
                    msg.textContent = nets.length + ' networks found.';
                } catch (e) { msg.textContent = 'Scan failed. Tap Rescan.'; }
            };
            x.send();
        }
        document.getElementById('rescan').addEventListener('click', scanNetworks);
        window.addEventListener('load', scanNetworks);
        document.getElementById('manualToggle').addEventListener('change', function() {
            document.getElementById('ssid_manual').style.display = this.checked ? 'block' : 'none';
        });
        // Show/hide password fields so users can verify what they typed.
        var eyes = document.querySelectorAll('.toggle-eye');
        for (var e = 0; e < eyes.length; e++) {
            eyes[e].addEventListener('click', function() {
                var input = document.getElementById(this.getAttribute('data-target'));
                if (!input) return;
                var show = input.type === 'password';
                input.type = show ? 'text' : 'password';
                this.textContent = show ? 'HIDE' : 'SHOW';
            });
        }

        document.getElementById('wifiForm').addEventListener('submit', function(e) {
            e.preventDefault();
            var status = document.getElementById('status');
            status.textContent = 'Saving settings...';
            status.className = 'status show';

            var xhr = new XMLHttpRequest();
            xhr.open('POST', '/save', true);
            xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
            xhr.onreadystatechange = function() {
                if (xhr.readyState === 4) {
                    if (xhr.status === 200) {
                        status.textContent = 'Saved! Rebooting...';
                        status.className = 'status show success';
                    } else {
                        status.textContent = 'Error: ' + xhr.responseText;
                        status.className = 'status show error';
                    }
                }
            };
            var ssid = document.getElementById('ssid').value;
            var manual = document.getElementById('ssid_manual').value.trim();
            if (manual.length > 0) ssid = manual;
            var password = document.getElementById('password').value;
            var regOrg = document.getElementById('reg_org').value;
            var regEmail = document.getElementById('reg_email').value;
            var regPassword = document.getElementById('reg_password').value;
            var regLocation = document.getElementById('reg_location').value;
            var claimCode = document.getElementById('claim_code').value;
            var deviceId = document.getElementById('device_id').value;
            var deviceKey = document.getElementById('device_key').value;
            var apiUrl = document.getElementById('api_url').value;
            var syncUrl = document.getElementById('sync_url').value;
            xhr.send('ssid=' + encodeURIComponent(ssid) +
                     '&password=' + encodeURIComponent(password) +
                     '&reg_org=' + encodeURIComponent(regOrg) +
                     '&reg_email=' + encodeURIComponent(regEmail) +
                     '&reg_password=' + encodeURIComponent(regPassword) +
                     '&reg_location=' + encodeURIComponent(regLocation) +
                     '&claim_code=' + encodeURIComponent(claimCode) +
                     '&device_id=' + encodeURIComponent(deviceId) +
                     '&device_key=' + encodeURIComponent(deviceKey) +
                     '&api_url=' + encodeURIComponent(apiUrl) +
                     '&sync_url=' + encodeURIComponent(syncUrl));
        });
    </script>
</body>
</html>
)rawliteral";

public:
    DTRDeskWiFiManager() : server(80) {}

    // Initialize WiFi Manager. The setup AP password is derived from this
    // chip's id (never a fixed factory value) unless one is passed explicitly.
    void begin(String apName = "DTRDesk-Setup", String apPass = "") {
        apSSID = apName;
        apPassword = (apPass.length() >= 8) ? apPass : deriveApPassword();

        LittleFS.begin();

        // Try to load saved credentials
        if (loadCredentials()) {
            Serial.printf("[WiFi] Found saved credentials for: %s\n", savedSSID.c_str());
            if (connectToWiFi()) {
                // Stay in STA mode if we already have credentials, OR if an org
                // login / claim code is staged (main.cpp exchanges it for
                // credentials on boot). Only re-open the portal when the device
                // is unclaimed AND has nothing staged to try.
                if (DeviceConfig::hasUsableKey() ||
                    DeviceConfig::hasPendingRegistration() ||
                    DeviceConfig::pendingClaim().length() > 0) {
                    apMode = false;
                    return;
                }
                Serial.println("[WiFi] Unclaimed with nothing staged - starting setup portal");
            } else {
                Serial.println("[WiFi] Failed to connect with saved credentials");
            }
        }

        // No credentials, connection failed, or device unclaimed - start AP mode
        startAPMode();
    }

    // Check if in AP mode
    bool isAPMode() {
        return apMode;
    }

    // Get current IP (AP or STA)
    String getIP() {
        if (apMode) {
            return WiFi.softAPIP().toString();
        }
        return WiFi.localIP().toString();
    }

    // Handle client requests (call in loop)
    void handleClient() {
        if (apMode) {
            if (dnsStarted) dnsServer.processNextRequest();
            server.handleClient();
        }
    }

    // Re-open the setup portal on demand (e.g. the claim code was wrong/expired
    // and the user needs to enter a new one). Safe to call more than once.
    void openSetupPortal() {
        if (apMode) return;
        startAPMode();
    }

    // Get saved SSID
    String getSSID() {
        return savedSSID;
    }

    // Setup AP identity (shown on the OLED during provisioning).
    String getAPSSID() {
        return apSSID;
    }

    String getAPPassword() {
        return apPassword;
    }

    // Clear saved WiFi credentials
    void clearCredentials() {
        LittleFS.remove("/wifi_config.json");
        savedSSID = "";
        savedPassword = "";
        Serial.println("[WiFi] Credentials cleared");
    }

private:
    // Per-device WPA2 password: "dtr" + the last 5 hex digits of the chip id
    // (8 chars, the WPA2 minimum). Fixed setup passwords are a known weakness.
    String deriveApPassword() {
        char buf[16];
        snprintf(buf, sizeof(buf), "dtr%05x", (unsigned)(ESP.getChipId() & 0xFFFFF));
        return String(buf);
    }

    // Minimal HTML-attribute escaping for values injected into the portal form.
    String htmlEscape(const String& in) {
        String out = in;
        out.replace("&", "&amp;");
        out.replace("\"", "&quot;");
        out.replace("<", "&lt;");
        out.replace(">", "&gt;");
        return out;
    }

    // Serve the portal with the device's current (runtime or compile-time) values.
    String renderPortal() {
        String html = String(portalHTML);
        html.replace("{{DEVICE_ID}}", htmlEscape(DeviceConfig::deviceId()));
        html.replace("{{API_URL}}", htmlEscape(DeviceConfig::apiUrl()));
        html.replace("{{SYNC_URL}}", htmlEscape(DeviceConfig::syncUrl()));
        return html;
    }

    // Load credentials from LittleFS. Parsed with ArduinoJson so quotes or
    // backslashes inside an SSID/password cannot corrupt or spoof the config.
    bool loadCredentials() {
        File file = LittleFS.open("/wifi_config.json", "r");
        if (!file) {
            return false;
        }

        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, file);
        file.close();
        if (err) {
            Serial.printf("[WiFi] Ignoring malformed /wifi_config.json: %s\n", err.c_str());
            return false;
        }

        savedSSID = doc["ssid"] | "";
        savedPassword = doc["password"] | "";
        return savedSSID.length() > 0;
    }

    // Save credentials to LittleFS (JSON-escaped via ArduinoJson).
    void saveCredentials(String ssid, String password) {
        StaticJsonDocument<512> doc;
        doc["ssid"] = ssid;
        doc["password"] = password;

        File file = LittleFS.open("/wifi_config.json", "w");
        if (file) {
            serializeJson(doc, file);
            file.close();
            Serial.println("[WiFi] Credentials saved to LittleFS");
        }
    }

    // Connect to saved WiFi
    bool connectToWiFi() {
        Serial.printf("[WiFi] Connecting to %s...\n", savedSSID.c_str());

        WiFi.mode(WIFI_STA);
        WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 30) {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println(F("\n[WiFi] Connected successfully!"));
            Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
            return true;
        }

        Serial.println(F("\n[WiFi] Connection failed"));
        return false;
    }

    // Start Access Point mode with captive portal
    void startAPMode() {
        apMode = true;
        Serial.printf("[WiFi] Starting AP mode: %s\n", apSSID.c_str());

        // AP_STA keeps any existing station connection while hosting the portal.
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(apSSID.c_str(), apPassword.c_str());

        delay(100);
        Serial.printf("[WiFi] AP IP Address: %s\n", WiFi.softAPIP().toString().c_str());

        // Hijack DNS so any hostname resolves here — this is what makes phones
        // pop the "Sign in to network" mini-browser automatically.
        dnsServer.start(53, "*", WiFi.softAPIP());
        dnsStarted = true;

        // Register the portal routes once (openSetupPortal may be called again).
        if (serverConfigured) {
            return;
        }
        server.on("/", [this]() {
            server.send(200, "text/html", renderPortal());
        });

        // Captive-portal detection endpoints: redirect to the portal instead of
        // answering their expected response, which triggers the OS mini-browser.
        const char* detectPaths[] = {
            "/generate_204", "/gen_204",                        // Android
            "/hotspot-detect.html", "/library/test/success.html", // Apple
            "/connecttest.txt", "/redirect", "/ncsi.txt"        // Windows
        };
        for (const char* path : detectPaths) {
            server.on(path, [this]() {
                server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
                server.send(302, "text/plain", "");
            });
        }

        // Nearby Wi-Fi scan for the captive portal's network picker.
        server.on("/scan", [this]() {
            int n = WiFi.scanNetworks(false, true); // blocking, include hidden SSIDs
            DynamicJsonDocument doc(6144);
            JsonArray arr = doc.createNestedArray("networks");
            int added = 0;
            for (int i = 0; i < n && added < 25; i++) {
                String ssid = WiFi.SSID(i);
                if (ssid.length() == 0) continue;
                JsonObject o = arr.createNestedObject();
                o["ssid"] = ssid;
                o["rssi"] = WiFi.RSSI(i);
                o["secure"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
                added++;
            }
            WiFi.scanDelete();
            String out;
            serializeJson(doc, out);
            server.send(200, "application/json", out);
        });

        server.on("/save", HTTP_POST, [this]() {
            String ssid = server.arg("ssid");
            String password = server.arg("password");

            if (ssid.length() == 0) {
                server.send(400, "text/plain", "SSID required");
                return;
            }

            saveCredentials(ssid, password);

            // PRIMARY: org credentials entered here are exchanged for this
            // gateway's own API key after reboot + Wi-Fi (main.cpp
            // tryRegisterGateway), binding it to that organization.
            String regOrg = server.arg("reg_org"); regOrg.trim(); regOrg.toUpperCase();
            String regEmail = server.arg("reg_email"); regEmail.trim();
            String regPassword = server.arg("reg_password");
            String regLocation = server.arg("reg_location"); regLocation.trim();

            if (regOrg.length() > 0 && regEmail.length() > 0 && regPassword.length() > 0) {
                if (!DeviceConfig::saveRegistration(regOrg, regEmail, regPassword, regLocation)) {
                    server.send(500, "text/plain", "Failed to save organization login");
                    return;
                }
                Serial.printf("[Device] Org registration staged for %s\n", regOrg.c_str());
            } else {
                // Alternative: a Claim Code (from the org portal) is exchanged
                // for credentials after the device reboots and Wi-Fi comes up.
                String claimCode = server.arg("claim_code"); claimCode.trim();
                claimCode.toUpperCase();
                if (claimCode.length() > 0) {
                    if (!DeviceConfig::saveClaimCode(claimCode)) {
                        server.send(500, "text/plain", "Failed to save claim code");
                        return;
                    }
                    Serial.printf("[Device] Claim code staged: %s\n", claimCode.c_str());
                }
            }

            // Advanced: persist manually supplied credentials (blank key = keep).
            String deviceId = server.arg("device_id"); deviceId.trim();
            String deviceKey = server.arg("device_key"); deviceKey.trim();
            String apiUrl = server.arg("api_url"); apiUrl.trim();
            String syncUrl = server.arg("sync_url"); syncUrl.trim();

            if (deviceId.length() > 0 && deviceKey.length() > 0) {
                if (DeviceConfig::save(deviceId, deviceKey, apiUrl, syncUrl)) {
                    Serial.printf("[Device] Provisioned device_id=%s\n", deviceId.c_str());
                } else {
                    server.send(500, "text/plain", "Failed to save device credentials");
                    return;
                }
            }

            server.send(200, "text/plain", "OK");
            delay(1000);
            ESP.restart();
        });

        server.onNotFound([this]() {
            // Redirect all unknown requests to captive portal
            server.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
            server.send(302);
        });

        server.begin();
        serverConfigured = true;
        Serial.println("[WiFi] Captive portal started");
    }
};

#endif // WIFI_MANAGER_H
