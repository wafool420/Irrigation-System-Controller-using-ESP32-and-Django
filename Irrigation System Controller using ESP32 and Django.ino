#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "esp_wifi.h"
#include <HTTPClient.h>

// =====================================================
// OUTPUT MODE TOGGLES
// =====================================================

// Keep this true while testing with LEDs
#define USE_LED_SIMULATION true

// Change this to true when using real relay + solenoid
#define USE_REAL_RELAY true

// =====================================================
// PIN SETTINGS
// =====================================================

// ESP32-C3 SuperMini:
// D3 = GPIO5
// D4 = GPIO6
#define SOLENOID_ON_LED_PIN   5   // Red LED = valve open / watering
#define SOLENOID_OFF_LED_PIN  6   // Yellow LED = valve closed / idle

// Change this depending on your real board.
// ESP32-C3 SuperMini usually does NOT have GPIO26.
#define RELAY_PIN 26

// Most relay modules are active LOW:
// LOW  = relay ON
// HIGH = relay OFF
#define RELAY_ACTIVE_LOW true

// Optional WiFi LEDs. Change pins if unused.
#define WIFI_OK_LED_PIN       7
#define WIFI_FAIL_LED_PIN     8

// =====================================================
// DJANGO CONTROL SETTINGS
// =====================================================

// Replace this with your PC/laptop IP address
String djangoCommandUrl = "http://192.168.0.100:8000/api/command/";

const unsigned long DJANGO_POLL_INTERVAL = 3000;
unsigned long lastDjangoPoll = 0;

// =====================================================
// SYSTEM STATES
// =====================================================

String currentMode = "OFF";
bool isValveOpen = false;

// AUTO mode timing
unsigned long autoClosedTime = 3600;  // seconds
unsigned long autoOpenTime = 20;      // seconds

bool autoValveOpenPhase = false;      // false = closed/waiting, true = watering/open
unsigned long autoPreviousMillis = 0;

// =====================================================
// ESP32 HOTSPOT / WIFI CONFIG
// =====================================================

String apSsid = "C3-Config";
String apPass = "12345678";

IPAddress apIP(192, 168, 4, 1);
const byte DNS_PORT = 53;

WebServer server(80);
DNSServer dns;
Preferences prefs;

String networksHTML = "";
bool inConfigMode = false;

// =====================================================
// OUTPUT CONTROL
// =====================================================

void setRelay(bool valveOpen) {
#if USE_REAL_RELAY
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, valveOpen ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, valveOpen ? HIGH : LOW);
  }
#endif
}

void setLedSimulation(bool valveOpen) {
#if USE_LED_SIMULATION
  digitalWrite(SOLENOID_ON_LED_PIN, valveOpen ? HIGH : LOW);
  digitalWrite(SOLENOID_OFF_LED_PIN, valveOpen ? LOW : HIGH);
#endif
}

void setValveOutput(bool valveOpen) {
  isValveOpen = valveOpen;

  setLedSimulation(valveOpen);
  setRelay(valveOpen);

  if (valveOpen) {
    Serial.println("VALVE STATE: OPEN / WATERING");
    Serial.println("LED STATE: RED ON, YELLOW OFF");
  } else {
    Serial.println("VALVE STATE: CLOSED / IDLE");
    Serial.println("LED STATE: RED OFF, YELLOW ON");
  }
}

// =====================================================
// WIFI LED STATUS
// =====================================================

void updateWiFiLeds() {
  if (WiFi.status() == WL_CONNECTED && !inConfigMode) {
    digitalWrite(WIFI_OK_LED_PIN, HIGH);
    digitalWrite(WIFI_FAIL_LED_PIN, LOW);
  } else {
    digitalWrite(WIFI_OK_LED_PIN, LOW);
    digitalWrite(WIFI_FAIL_LED_PIN, HIGH);
  }
}

// =====================================================
// ROUTER WIFI MEMORY
// =====================================================

void saveWifi(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  Serial.println("Router WiFi saved.");
}

bool loadSavedWifi(String &ssid, String &pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();

  ssid.trim();
  pass.trim();

  if (ssid.length() == 0) {
    Serial.println("No saved router WiFi.");
    return false;
  }

  Serial.print("Loaded saved router WiFi: ");
  Serial.println(ssid);
  return true;
}

void clearSavedWifi() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  Serial.println("Saved router WiFi cleared.");
}

// =====================================================
// ESP32 HOTSPOT MEMORY
// =====================================================

void loadApConfig() {
  prefs.begin("ap", true);
  apSsid = prefs.getString("ssid", "C3-Config");
  apPass = prefs.getString("pass", "12345678");
  prefs.end();

  apSsid.trim();
  apPass.trim();

  if (apSsid.length() == 0) {
    apSsid = "C3-Config";
  }

  if (apPass.length() < 8) {
    apPass = "12345678";
  }

  Serial.println("Loaded ESP32 hotspot config:");
  Serial.print("Hotspot SSID: ");
  Serial.println(apSsid);
  Serial.print("Hotspot Password: ");
  Serial.println(apPass);
}

void saveApConfig(const String &ssid, const String &pass) {
  String newSsid = ssid;
  String newPass = pass;

  newSsid.trim();
  newPass.trim();

  if (newSsid.length() == 0) {
    newSsid = "C3-Config";
  }

  if (newPass.length() < 8) {
    newPass = "12345678";
  }

  prefs.begin("ap", false);
  prefs.putString("ssid", newSsid);
  prefs.putString("pass", newPass);
  prefs.end();

  apSsid = newSsid;
  apPass = newPass;

  Serial.println("ESP32 hotspot config saved.");
}

// =====================================================
// WIFI CONNECT
// =====================================================

bool connectWiFiWith(const String &ssid, const String &pass, bool keepAP) {
  WiFi.disconnect(true, true);
  delay(300);

  WiFi.mode(keepAP ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(78);

  const int MAX_ATTEMPTS = 3;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    Serial.print("Connecting to ");
    Serial.print(ssid);
    Serial.print(" attempt ");
    Serial.println(attempt);

    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      updateWiFiLeds();
      Serial.print(".");
      delay(500);
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Router WiFi connected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());

      updateWiFiLeds();
      return true;
    }

    Serial.println("Router WiFi failed.");
    updateWiFiLeds();
    delay(1000);
  }

  updateWiFiLeds();
  return false;
}

bool connectSavedWifi() {
  String ssid, pass;

  if (!loadSavedWifi(ssid, pass)) {
    return false;
  }

  return connectWiFiWith(ssid, pass, false);
}

// =====================================================
// WIFI SCAN
// =====================================================

void buildNetworksList() {
  Serial.println("Scanning WiFi networks...");

  networksHTML = "";

  int n = WiFi.scanNetworks();

  if (n <= 0) {
    networksHTML = "<option value=''>No networks found</option>";
    Serial.println("No networks found.");
  } else {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);

      if (ssid.length() == 0) continue;

      networksHTML += "<option value='";
      networksHTML += ssid;
      networksHTML += "'>";
      networksHTML += ssid;
      networksHTML += "</option>";
    }
  }

  WiFi.scanDelete();
}

// =====================================================
// CAPTIVE PORTAL REDIRECT
// =====================================================

void redirectToPortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// =====================================================
// HTML PAGES
// =====================================================

String buildPortalHtml() {
  if (networksHTML.length() == 0) {
    buildNetworksList();
  }

  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>ESP32-C3 WiFi Config</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;}"
      "h2{margin-top:0;}"
      "h3{margin-bottom:5px;margin-top:22px;}"
      "label{display:block;margin-top:10px;font-size:13px;color:#333;}"
      "input,select{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid #ccc;border-radius:10px;font-size:14px;}"
      "button,a{display:block;width:100%;box-sizing:border-box;margin-top:12px;padding:12px;border-radius:12px;border:none;background:#111;color:#fff;"
      "text-decoration:none;text-align:center;font-weight:800;font-size:14px;}"
      "a{background:#6b7280;}"
      ".red{background:#dc2626;}"
      ".status{background:#f1f1f1;border-radius:12px;padding:10px;margin-bottom:12px;font-size:14px;}"
      ".small{font-size:12px;color:#666;margin-top:5px;}"
      ".line{height:1px;background:#e5e5e5;margin:22px 0;}"
    "</style></head><body>"
    "<div class='c'>"
      "<h2>WiFi Setup</h2>"
      "<div class='status'>Connected to ESP32 hotspot: <b>" + apSsid + "</b></div>"

      "<form method='POST' action='/save'>"
        "<h3>Router WiFi</h3>"
        "<label>WiFi SSID</label>"
        "<select name='ssid'>" + networksHTML + "</select>"

        "<label>WiFi Password</label>"
        "<input type='password' name='pass' placeholder='Enter router WiFi password'/>"

        "<button type='submit'>Connect & Save Router WiFi</button>"
      "</form>"

      "<div class='line'></div>"

      "<form method='POST' action='/save_ap'>"
        "<h3>ESP32 Hotspot Settings</h3>"

        "<label>ESP32 Hotspot Name</label>"
        "<input type='text' name='ap_ssid' value='" + apSsid + "' placeholder='C3-Config'/>"

        "<label>ESP32 Hotspot Password</label>"
        "<input type='password' name='ap_pass' value='" + apPass + "' placeholder='Minimum 8 characters'/>"
        "<div class='small'>Password must be at least 8 characters. This applies after reboot.</div>"

        "<button type='submit'>Save Hotspot Name & Password</button>"
      "</form>"

      "<a href='/rescan'>Rescan Networks</a>"
      "<a class='red' href='/reset' onclick=\"return confirm('Clear saved router WiFi?');\">Clear Saved Router WiFi</a>"
    "</div>"
    "</body></html>";

  return html;
}

String buildConnectedHtml(const String &ssid, const String &ip) {
  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>Connected</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;text-align:center;}"
      "h2{margin-top:0;color:#16a34a;}"
      "code{background:#f1f1f1;padding:4px 8px;border-radius:8px;}"
      "a{display:block;width:100%;box-sizing:border-box;margin-top:12px;padding:12px;border-radius:12px;background:#111;color:#fff;"
      "text-decoration:none;text-align:center;font-weight:800;font-size:14px;}"
      ".gray{background:#6b7280;}"
    "</style></head><body>"
    "<div class='c'>"
      "<h2>Connected ✅</h2>"
      "<p>ESP32 successfully connected to router WiFi.</p>"
      "<p><b>Router WiFi:</b> <code>" + ssid + "</code></p>"
      "<p><b>ESP32 IP:</b> <code>" + ip + "</code></p>"
      "<a href='http://" + ip + "/'>Open ESP32 Status Page</a>"
      "<a class='gray' href='/'>Back to Config Page</a>"
    "</div>"
    "</body></html>";

  return html;
}

String buildFailedHtml() {
  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>Connection Failed</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;text-align:center;}"
      "h2{margin-top:0;color:#dc2626;}"
      "a{display:block;width:100%;box-sizing:border-box;margin-top:12px;padding:12px;border-radius:12px;background:#111;color:#fff;"
      "text-decoration:none;text-align:center;font-weight:800;font-size:14px;}"
    "</style></head><body>"
    "<div class='c'>"
      "<h2>Connection Failed ❌</h2>"
      "<p>ESP32 could not connect to the selected router WiFi.</p>"
      "<p>Please check the password and try again.</p>"
      "<a href='/'>Try Again</a>"
    "</div>"
    "</body></html>";

  return html;
}

String buildHotspotSavedHtml() {
  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>Hotspot Updated</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;text-align:center;}"
      "h2{margin-top:0;color:#16a34a;}"
      "code{background:#f1f1f1;padding:4px 8px;border-radius:8px;}"
    "</style></head><body>"
    "<div class='c'>"
      "<h2>Hotspot Updated ✅</h2>"
      "<p>New ESP32 hotspot name/password saved.</p>"
      "<p><b>New Hotspot Name:</b> <code>" + apSsid + "</code></p>"
      "<p>The ESP32 will reboot now. Reconnect using the new hotspot password.</p>"
    "</div>"
    "</body></html>";

  return html;
}

String buildHotspotPasswordErrorHtml() {
  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>Password Too Short</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;text-align:center;}"
      "h2{margin-top:0;color:#dc2626;}"
      "a{display:block;width:100%;box-sizing:border-box;margin-top:12px;padding:12px;border-radius:12px;background:#111;color:#fff;"
      "text-decoration:none;text-align:center;font-weight:800;font-size:14px;}"
    "</style></head><body>"
    "<div class='c'>"
      "<h2>Password Too Short ❌</h2>"
      "<p>ESP32 hotspot password must be at least 8 characters.</p>"
      "<a href='/'>Go Back</a>"
    "</div>"
    "</body></html>";

  return html;
}

String buildStatusHtml() {
  String valveText = isValveOpen ? "OPEN / WATERING" : "CLOSED / IDLE";

  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>ESP32-C3 Status</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;}"
      "h2{margin-top:0;color:#16a34a;}"
      "code{background:#f1f1f1;padding:3px 7px;border-radius:8px;}"
      ".box{background:#f8fafc;border:1px solid #e5e7eb;border-radius:12px;padding:12px;margin-top:12px;}"
      "a{display:block;width:100%;box-sizing:border-box;margin-top:12px;padding:12px;border-radius:12px;background:#dc2626;color:#fff;"
      "text-decoration:none;text-align:center;font-weight:800;font-size:14px;}"
    "</style></head><body>"
    "<div class='c'>"
      "<h2>ESP32 Connected ✅</h2>"
      "<p><b>Router WiFi:</b> <code>" + WiFi.SSID() + "</code></p>"
      "<p><b>ESP32 IP:</b> <code>" + WiFi.localIP().toString() + "</code></p>"
      "<p><b>Saved Hotspot Name:</b> <code>" + apSsid + "</code></p>"

      "<div class='box'>"
        "<p><b>Django Mode:</b> <code>" + currentMode + "</code></p>"
        "<p><b>Valve State:</b> <code>" + valveText + "</code></p>"
        "<p><b>Auto Timer:</b> <code>" + String(autoClosedTime) + "s CLOSED / " + String(autoOpenTime) + "s OPEN</code></p>"
        "<p><b>LED Simulation:</b> <code>" + String(USE_LED_SIMULATION ? "ON" : "OFF") + "</code></p>"
        "<p><b>Real Relay:</b> <code>" + String(USE_REAL_RELAY ? "ON" : "OFF") + "</code></p>"
      "</div>"

      "<a href='/reset' onclick=\"return confirm('Clear saved router WiFi?');\">Clear Saved Router WiFi</a>"
    "</div>"
    "</body></html>";

  return html;
}

// =====================================================
// SERVER HANDLERS
// =====================================================

void handlePortal() {
  server.send(200, "text/html", buildPortalHtml());
}

void handleStatusPage() {
  server.send(200, "text/html", buildStatusHtml());
}

void handleRescan() {
  buildNetworksList();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Rescanning...");
}

void handleSaveConfig() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  ssid.trim();
  pass.trim();

  if (ssid.length() == 0) {
    server.send(200, "text/plain", "No router WiFi SSID selected.");
    return;
  }

  bool ok = connectWiFiWith(ssid, pass, true);

  if (ok) {
    saveWifi(ssid, pass);

    String ip = WiFi.localIP().toString();

    server.send(200, "text/html", buildConnectedHtml(ssid, ip));

    delay(3000);
    ESP.restart();
  } else {
    server.send(200, "text/html", buildFailedHtml());
  }
}

void handleSaveAP() {
  String newApSsid = server.arg("ap_ssid");
  String newApPass = server.arg("ap_pass");

  newApSsid.trim();
  newApPass.trim();

  if (newApPass.length() < 8) {
    server.send(200, "text/html", buildHotspotPasswordErrorHtml());
    return;
  }

  saveApConfig(newApSsid, newApPass);

  server.send(200, "text/html", buildHotspotSavedHtml());

  delay(2500);
  ESP.restart();
}

void handleReset() {
  clearSavedWifi();

  server.send(200, "text/plain", "Saved router WiFi cleared. Rebooting...");
  delay(800);

  ESP.restart();
}

void handleNotFound() {
  if (inConfigMode) {
    redirectToPortal();
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// =====================================================
// CONFIG AP MODE
// =====================================================

void startConfigAP() {
  Serial.println("Starting config AP...");
  inConfigMode = true;

  updateWiFiLeds();
  setValveOutput(false);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(78);

  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  bool apStarted = WiFi.softAP(apSsid.c_str(), apPass.c_str(), 1, 0, 4);

  if (apStarted) {
    Serial.println("ESP32 hotspot started.");
  } else {
    Serial.println("ESP32 hotspot failed to start.");
  }

  Serial.print("Hotspot SSID: ");
  Serial.println(apSsid);
  Serial.print("Hotspot Password: ");
  Serial.println(apPass);
  Serial.print("Hotspot IP: ");
  Serial.println(apIP);

  dns.start(DNS_PORT, "*", apIP);

  buildNetworksList();

  server.on("/", handlePortal);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/save_ap", HTTP_POST, handleSaveAP);
  server.on("/rescan", handleRescan);
  server.on("/reset", handleReset);

  server.on("/generate_204", redirectToPortal);
  server.on("/gen_204", redirectToPortal);
  server.on("/mobile/status.php", redirectToPortal);

  server.on("/hotspot-detect.html", handlePortal);
  server.on("/library/test/success.html", handlePortal);

  server.on("/ncsi.txt", redirectToPortal);
  server.on("/connecttest.txt", redirectToPortal);
  server.on("/redirect", redirectToPortal);
  server.on("/fwlink", redirectToPortal);

  server.on("/success.txt", redirectToPortal);
  server.on("/canonical.html", redirectToPortal);

  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("Connect to ESP32 hotspot, then open:");
  Serial.println("http://192.168.4.1/");
}

// =====================================================
// CONNECTED MODE
// =====================================================

void startConnectedServer() {
  inConfigMode = false;
  dns.stop();

  updateWiFiLeds();

  server.on("/", handleStatusPage);
  server.on("/reset", handleReset);
  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("Status page started.");
  Serial.print("Open: http://");
  Serial.println(WiFi.localIP());
}

// =====================================================
// SIMPLE JSON PARSING
// =====================================================

String getJsonStringValue(String json, String key) {
  String searchKey = "\"" + key + "\"";
  int keyIndex = json.indexOf(searchKey);

  if (keyIndex == -1) return "";

  int colonIndex = json.indexOf(":", keyIndex);
  if (colonIndex == -1) return "";

  int firstQuote = json.indexOf("\"", colonIndex);
  if (firstQuote == -1) return "";

  int secondQuote = json.indexOf("\"", firstQuote + 1);
  if (secondQuote == -1) return "";

  return json.substring(firstQuote + 1, secondQuote);
}

unsigned long getJsonNumberValue(String json, String key) {
  String searchKey = "\"" + key + "\":";
  int startIndex = json.indexOf(searchKey);

  if (startIndex == -1) {
    return 0;
  }

  startIndex += searchKey.length();
  int endIndex = json.indexOf(",", startIndex);

  if (endIndex == -1) {
    endIndex = json.indexOf("}", startIndex);
  }

  if (endIndex == -1) {
    return 0;
  }

  String value = json.substring(startIndex, endIndex);
  value.trim();

  return value.toInt();
}

// =====================================================
// DJANGO COMMAND FETCH
// =====================================================

void fetchCommandFromDjango() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;

  Serial.print("Asking Django: ");
  Serial.println(djangoCommandUrl);

  http.begin(djangoCommandUrl);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    Serial.print("Django response: ");
    Serial.println(payload);

    String newMode = getJsonStringValue(payload, "mode");
    unsigned long newAutoClosedTime = getJsonNumberValue(payload, "auto_closed_time");
    unsigned long newAutoOpenTime = getJsonNumberValue(payload, "auto_open_time");

    if (newAutoClosedTime > 0) {
      autoClosedTime = newAutoClosedTime;
    }

    if (newAutoOpenTime > 0) {
      autoOpenTime = newAutoOpenTime;
    }

    if (newMode == "ON" || newMode == "OFF" || newMode == "AUTO") {
      if (newMode != currentMode) {
        Serial.print("Mode changed to: ");
        Serial.println(newMode);

        currentMode = newMode;

        autoValveOpenPhase = false;
        autoPreviousMillis = millis();

        if (currentMode == "AUTO") {
          setValveOutput(false);
          Serial.println("AUTO: Starting CLOSED/IDLE phase");
        }
      }
    }

    Serial.print("Timer setting: ");
    Serial.print(autoClosedTime);
    Serial.print(" seconds CLOSED/OFF, ");
    Serial.print(autoOpenTime);
    Serial.println(" seconds OPEN/ON");

  } else {
    Serial.print("Failed to contact Django. HTTP code: ");
    Serial.println(httpCode);
  }

  http.end();
}

// =====================================================
// MODE HANDLERS
// =====================================================

void handleForceMode() {
  if (currentMode == "ON") {
    setValveOutput(true);
  } else if (currentMode == "OFF") {
    setValveOutput(false);
  }
}

void handleAutoMode() {
  unsigned long currentMillis = millis();
  unsigned long closedTimeMs = autoClosedTime * 1000UL;
  unsigned long openTimeMs = autoOpenTime * 1000UL;

  if (!autoValveOpenPhase) {
    // CLOSED / IDLE phase
    if (currentMillis - autoPreviousMillis >= closedTimeMs) {
      autoValveOpenPhase = true;
      autoPreviousMillis = currentMillis;
      setValveOutput(true);

      Serial.print("AUTO: Valve OPEN for ");
      Serial.print(autoOpenTime);
      Serial.println(" seconds");
    }
  } else {
    // OPEN / WATERING phase
    if (currentMillis - autoPreviousMillis >= openTimeMs) {
      autoValveOpenPhase = false;
      autoPreviousMillis = currentMillis;
      setValveOutput(false);

      Serial.print("AUTO: Valve CLOSED for ");
      Serial.print(autoClosedTime);
      Serial.println(" seconds");
    }
  }
}

void handleDjangoControl() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastDjangoPoll >= DJANGO_POLL_INTERVAL) {
    lastDjangoPoll = currentMillis;
    fetchCommandFromDjango();
  }

  if (currentMode == "AUTO") {
    handleAutoMode();
  } else {
    handleForceMode();
  }
}

// =====================================================
// SETUP / LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(600);

  pinMode(WIFI_OK_LED_PIN, OUTPUT);
  pinMode(WIFI_FAIL_LED_PIN, OUTPUT);

#if USE_LED_SIMULATION
  pinMode(SOLENOID_ON_LED_PIN, OUTPUT);
  pinMode(SOLENOID_OFF_LED_PIN, OUTPUT);
#endif

#if USE_REAL_RELAY
  pinMode(RELAY_PIN, OUTPUT);

  // Start relay OFF for safety
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, HIGH);
  } else {
    digitalWrite(RELAY_PIN, LOW);
  }
#endif

  digitalWrite(WIFI_OK_LED_PIN, LOW);
  digitalWrite(WIFI_FAIL_LED_PIN, LOW);

  setValveOutput(false);

  Serial.println();
  Serial.println("=== ESP32-C3 WiFi Captive Portal + Django Irrigation Control ===");

  Serial.print("LED Simulation: ");
  Serial.println(USE_LED_SIMULATION ? "ON" : "OFF");

  Serial.print("Real Relay: ");
  Serial.println(USE_REAL_RELAY ? "ON" : "OFF");

  loadApConfig();

  if (connectSavedWifi()) {
    startConnectedServer();
  } else {
    startConfigAP();
  }

  updateWiFiLeds();
}

void loop() {
  updateWiFiLeds();

  if (inConfigMode) {
    dns.processNextRequest();
    server.handleClient();
    delay(10);
    return;
  }

  server.handleClient();

  handleDjangoControl();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Router WiFi lost. Trying saved WiFi again...");

    updateWiFiLeds();

    if (!connectSavedWifi()) {
      Serial.println("Reconnect failed. Starting config portal.");
      startConfigAP();
    }
  }

  delay(1000);
}