#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <DNSServer.h>

// =====================
// RELAY SETTINGS
// =====================
const int RELAY_PIN = 26;
const bool RELAY_ACTIVE_LOW = true;

// =====================
// DJANGO SETTINGS
// =====================
String djangoCommandUrl = "http://10.141.92.83:8000/api/command/";

const unsigned long DJANGO_POLL_INTERVAL = 3000;
unsigned long lastDjangoPoll = 0;

// =====================
// WIFI PORTAL SETTINGS
// =====================
String apSsid = "Irrigation-Config";
String apPass = "12345678";

IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
const byte DNS_PORT = 53;

WebServer server(80);
Preferences prefs;

// The portal is now always active
bool portalAlwaysOn = true;

// =====================
// WIFI RECONNECT SETTINGS
// =====================
const unsigned long WIFI_RECONNECT_INTERVAL = 10000;
unsigned long lastWifiReconnectAttempt = 0;

// =====================
// STATE
// =====================
String currentMode = "OFF";

unsigned long autoClosedTime = 3600;
unsigned long autoOpenTime = 20;

bool valveOpen = false;
bool autoOpenPhase = false;
unsigned long autoPreviousMillis = 0;

// =====================
// RELAY CONTROL
// =====================
void setRelay(bool openValve) {
  valveOpen = openValve;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, openValve ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, openValve ? HIGH : LOW);
  }

  Serial.print("VALVE: ");
  Serial.println(openValve ? "OPEN / WATERING" : "CLOSED / IDLE");
}

// =====================
// WIFI MEMORY
// =====================
void saveWifi(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  Serial.println("WiFi saved.");
}

bool loadWifi(String &ssid, String &pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();

  ssid.trim();
  pass.trim();

  return ssid.length() > 0;
}

void clearWifi() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  Serial.println("Saved WiFi cleared.");
}

// =====================
// HOTSPOT PASSWORD MEMORY
// =====================
void saveHotspotPassword(const String &newPass) {
  prefs.begin("portal", false);
  prefs.putString("ap_pass", newPass);
  prefs.end();

  apPass = newPass;
  apPass.trim();

  Serial.println("Hotspot password saved:");
  Serial.println(apPass);
}

void loadHotspotPassword() {
  prefs.begin("portal", true);
  String savedPass = prefs.getString("ap_pass", "");
  prefs.end();

  savedPass.trim();

  if (savedPass.length() >= 8) {
    apPass = savedPass;
  }

  Serial.print("Current hotspot password: ");
  Serial.println(apPass);
}

// =====================
// DJANGO URL MEMORY
// =====================
void saveDjangoUrl(const String &url) {
  prefs.begin("django", false);
  prefs.putString("url", url);
  prefs.end();

  djangoCommandUrl = url;
  djangoCommandUrl.trim();

  Serial.println("Django API URL saved:");
  Serial.println(djangoCommandUrl);
}

void loadDjangoUrl() {
  prefs.begin("django", true);
  String savedUrl = prefs.getString("url", "");
  prefs.end();

  savedUrl.trim();

  if (savedUrl.length() > 0) {
    djangoCommandUrl = savedUrl;
  }

  Serial.print("Current Django API URL: ");
  Serial.println(djangoCommandUrl);
}

// =====================
// SIMPLE JSON PARSER
// =====================
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
  if (startIndex == -1) return 0;

  startIndex += searchKey.length();

  int endIndex = json.indexOf(",", startIndex);
  if (endIndex == -1) {
    endIndex = json.indexOf("}", startIndex);
  }

  if (endIndex == -1) return 0;

  String value = json.substring(startIndex, endIndex);
  value.trim();

  return value.toInt();
}

// =====================
// WIFI CONNECT
// =====================
void startAlwaysOnHotspot() {
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  bool apStarted = WiFi.softAP(apSsid.c_str(), apPass.c_str());

  if (apStarted) {
    Serial.println("Always-on hotspot started.");
    Serial.print("Hotspot name: ");
    Serial.println(apSsid);
    Serial.print("Hotspot password: ");
    Serial.println(apPass);
    Serial.println("Portal: http://192.168.4.1");
  } else {
    Serial.println("Failed to start hotspot.");
  }

  dnsServer.start(DNS_PORT, "*", apIP);
}

bool connectWiFi(const String &ssid, const String &pass) {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    Serial.print(".");
    delay(300);
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Router WiFi connected.");
    Serial.print("ESP32 router IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Router WiFi failed.");
  return false;
}

bool connectSavedWifi() {
  String ssid, pass;

  if (!loadWifi(ssid, pass)) {
    Serial.println("No saved router WiFi.");
    return false;
  }

  return connectWiFi(ssid, pass);
}

void reconnectWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();

  if (now - lastWifiReconnectAttempt < WIFI_RECONNECT_INTERVAL) return;

  lastWifiReconnectAttempt = now;

  Serial.println("Router WiFi disconnected. Keeping hotspot ON.");
  Serial.println("Trying to reconnect to saved WiFi...");

  currentMode = "OFF";
  setRelay(false);

  String ssid, pass;

  if (loadWifi(ssid, pass)) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    Serial.println("No saved WiFi. Use the hotspot portal to configure.");
  }
}

// =====================
// HTML STYLE
// =====================
String pageStyle() {
  return
    "<style>"
    "body{font-family:Arial;background:#f3f4f6;margin:0;padding:20px;}"
    ".card{max-width:460px;margin:auto;background:white;padding:20px;border-radius:14px;box-shadow:0 8px 20px rgba(0,0,0,.08);}"
    "h2{text-align:center;margin-top:0;}"
    "label{font-weight:bold;display:block;margin-top:12px;}"
    "input{width:100%;box-sizing:border-box;padding:12px;border:1px solid #ccc;border-radius:8px;margin-top:6px;}"
    "button,a{display:block;width:100%;box-sizing:border-box;margin-top:14px;padding:12px;border-radius:8px;border:0;text-align:center;text-decoration:none;font-weight:bold;}"
    "button{background:#111827;color:white;}"
    ".danger{background:#dc2626;color:white;}"
    ".secondary{background:#2563eb;color:white;}"
    ".green{background:#16a34a;color:white;}"
    ".small{font-size:13px;color:#555;text-align:center;line-height:1.5;}"
    "code{background:#eee;padding:3px 6px;border-radius:5px;word-break:break-all;}"
    "</style>";
}

// =====================
// HTML PAGES
// =====================
String portalPage() {
  String routerStatus = WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED";
  String routerIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "No router IP";
  String valveText = valveOpen ? "OPEN / WATERING" : "CLOSED / IDLE";

  return
    "<!doctype html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>Irrigation Portal</title>"
    + pageStyle() +
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h2>Irrigation Portal</h2>"

    "<p><b>Router WiFi:</b> <code>" + routerStatus + "</code></p>"
    "<p><b>Router IP:</b> <code>" + routerIp + "</code></p>"
    "<p><b>Hotspot IP:</b> <code>192.168.4.1</code></p>"
    "<p><b>Hotspot Name:</b> <code>" + apSsid + "</code></p>"
    "<p><b>Hotspot Password:</b> <code>" + apPass + "</code></p>"
    "<p><b>Django API:</b><br><code>" + djangoCommandUrl + "</code></p>"
    "<p><b>Mode:</b> <code>" + currentMode + "</code></p>"
    "<p><b>Valve:</b> <code>" + valveText + "</code></p>"

    "<hr>"

    "<form method='POST' action='/save-wifi'>"
    "<label>Router WiFi Name</label>"
    "<input name='ssid' placeholder='Enter WiFi SSID' required>"

    "<label>Router WiFi Password</label>"
    "<input name='pass' type='password' placeholder='Enter WiFi password'>"

    "<button type='submit'>Save Router WiFi and Restart</button>"
    "</form>"

    "<form method='POST' action='/save-api'>"
    "<label>Change Django API URL</label>"
    "<input name='django_url' value='" + djangoCommandUrl + "' required>"
    "<button class='secondary' type='submit'>Save API URL</button>"
    "</form>"

    "<form method='POST' action='/save-hotspot'>"
    "<label>Change ESP32 Hotspot Password</label>"
    "<input id='apPassInput' name='ap_pass' type='password' value='" + apPass + "' minlength='8' required>"
    "<button type='button' class='secondary' onclick='toggleApPassword()'>Show / Hide Password</button>"
    "<button class='green' type='submit'>Save Hotspot Password and Restart</button>"
    "</form>"

    "<a class='danger' href='/clear'>Clear Saved Router WiFi</a>"
    "</div>"
    "<script>"
    "function toggleApPassword(){"
    "  var input=document.getElementById('apPassInput');"
    "  if(input.type==='password'){"
    "    input.type='text';"
    "  }else{"
    "    input.type='password';"
    "  }"
    "}"
    "</script>"
    "</body>"
    "</html>";
}

// =====================
// CAPTIVE PORTAL SUPPORT
// =====================
void redirectToPortal() {
  server.sendHeader("Location", String("http://") + apIP.toString(), true);
  server.send(302, "text/plain", "");
}

void handleCaptivePortal() {
  redirectToPortal();
}

// =====================
// SERVER HANDLERS
// =====================
void handleRoot() {
  server.send(200, "text/html", portalPage());
}

void handleSaveWifi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  ssid.trim();
  pass.trim();

  if (ssid.length() == 0) {
    server.send(200, "text/plain", "No SSID entered.");
    return;
  }

  saveWifi(ssid, pass);

  server.send(
    200,
    "text/html",
    "<h2>Router WiFi saved. Restarting...</h2><p>The ESP32 hotspot will turn on again after restart.</p>"
  );

  delay(1000);
  ESP.restart();
}

void handleSaveApi() {
  String newDjangoUrl = server.arg("django_url");
  newDjangoUrl.trim();

  if (newDjangoUrl.length() == 0) {
    server.send(200, "text/plain", "No Django API URL entered.");
    return;
  }

  saveDjangoUrl(newDjangoUrl);

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSaveHotspot() {
  String newApPass = server.arg("ap_pass");
  newApPass.trim();

  if (newApPass.length() < 8) {
    server.send(200, "text/plain", "Hotspot password must be at least 8 characters.");
    return;
  }

  saveHotspotPassword(newApPass);

  server.send(
    200,
    "text/html",
    "<h2>Hotspot password saved. Restarting...</h2><p>Reconnect using the new hotspot password.</p>"
  );

  delay(1000);
  ESP.restart();
}

void handleClear() {
  clearWifi();

  server.send(
    200,
    "text/html",
    "<h2>Router WiFi cleared. Restarting...</h2><p>Use the ESP32 hotspot to configure WiFi again.</p>"
  );

  delay(1000);
  ESP.restart();
}

void startServer() {
  server.on("/", handleRoot);

  server.on("/save-wifi", HTTP_POST, handleSaveWifi);
  server.on("/save-api", HTTP_POST, handleSaveApi);
  server.on("/save-hotspot", HTTP_POST, handleSaveHotspot);
  server.on("/clear", handleClear);

  // Android, Windows, and Apple captive portal checks
  server.on("/generate_204", redirectToPortal);
  server.on("/fwlink", redirectToPortal);
  server.on("/hotspot-detect.html", redirectToPortal);
  server.on("/library/test/success.html", redirectToPortal);
  server.on("/ncsi.txt", redirectToPortal);
  server.on("/connecttest.txt", redirectToPortal);

  server.onNotFound(handleCaptivePortal);

  server.begin();

  Serial.println("Web server started.");
}

// =====================
// DJANGO FETCH
// =====================
void fetchCommandFromDjango() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Router WiFi disconnected. Cannot ask Django.");
    return;
  }

  HTTPClient http;
  http.setTimeout(3000);

  Serial.print("Asking Django: ");
  Serial.println(djangoCommandUrl);

  http.begin(djangoCommandUrl);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    Serial.print("Django response: ");
    Serial.println(payload);

    String newMode = getJsonStringValue(payload, "mode");
    unsigned long newClosedTime = getJsonNumberValue(payload, "auto_closed_time");
    unsigned long newOpenTime = getJsonNumberValue(payload, "auto_open_time");

    if (newClosedTime > 0) autoClosedTime = newClosedTime;
    if (newOpenTime > 0) autoOpenTime = newOpenTime;

    if (newMode == "ON" || newMode == "OFF" || newMode == "AUTO") {
      if (newMode != currentMode) {
        currentMode = newMode;

        Serial.print("Mode changed to: ");
        Serial.println(currentMode);

        autoOpenPhase = false;
        autoPreviousMillis = millis();

        if (currentMode == "AUTO") {
          setRelay(false);
          Serial.println("AUTO: starting closed/waiting phase");
        }
      }
    }

  } else {
    Serial.print("Django request failed. HTTP code: ");
    Serial.println(httpCode);
  }

  http.end();
}

// =====================
// MODE HANDLERS
// =====================
void handleForceMode() {
  if (currentMode == "ON") {
    if (!valveOpen) setRelay(true);
  } else if (currentMode == "OFF") {
    if (valveOpen) setRelay(false);
  }
}

void handleAutoMode() {
  unsigned long now = millis();

  unsigned long closedMs = autoClosedTime * 1000UL;
  unsigned long openMs = autoOpenTime * 1000UL;

  if (!autoOpenPhase) {
    if (now - autoPreviousMillis >= closedMs) {
      autoOpenPhase = true;
      autoPreviousMillis = now;
      setRelay(true);

      Serial.print("AUTO: valve opened for ");
      Serial.print(autoOpenTime);
      Serial.println(" seconds");
    }
  } else {
    if (now - autoPreviousMillis >= openMs) {
      autoOpenPhase = false;
      autoPreviousMillis = now;
      setRelay(false);

      Serial.print("AUTO: valve closed for ");
      Serial.print(autoClosedTime);
      Serial.println(" seconds");
    }
  }
}

void handleControl() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastDjangoPoll >= DJANGO_POLL_INTERVAL) {
      lastDjangoPoll = now;
      fetchCommandFromDjango();
    }

    if (currentMode == "AUTO") {
      handleAutoMode();
    } else {
      handleForceMode();
    }
  } else {
    currentMode = "OFF";
    setRelay(false);
  }
}

// =====================
// SETUP / LOOP
// =====================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  loadDjangoUrl();
  loadHotspotPassword();

  Serial.println();
  Serial.println("=== ESP32 Irrigation Controller with Always-On Portal ===");

  startAlwaysOnHotspot();
  startServer();

  connectSavedWifi();

  Serial.println();
  Serial.println("Portal is always available at:");
  Serial.println("http://192.168.4.1");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Also available through router IP:");
    Serial.println(WiFi.localIP());
  }
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  reconnectWiFiIfNeeded();
  handleControl();

  delay(20);
}