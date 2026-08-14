#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Update.h>
#include <HardwareSerial.h>
#include <DFPlayerMini_Fast.h>
#include <NTPClient.h>
#include <WiFiUdp.h> 
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "dgus.h"
#include "qrcode.h"

// ===================== OTA CONFIG =====================
#define CURRENT_VERSION_STR  "1.1.0"
#define OTA_BASE_URL         "https://freshpod-ota-r3b9.onrender.com/firmware/"
// ======================================================

#define WIFI_SSID          "Sushrut's iPhone"
#define WIFI_PASSWORD      "lionellcasa"
#define ADMIN_AP_PASSWORD  "Freshpod@2026"

// Firebase configurations synced from backend .env
#define FIREBASE_API_KEY   "AIzaSyA03ON8h3-Txnn12LsqwinTZMzDbddd1nc"
#define FIREBASE_HOST      "freshpod-901ed-default-rtdb.firebaseio.com"
#define MACHINE_ID         "FP_MACHINE_01"
#define NTP_OFFSET         19800
#define TOKEN_REFRESH_MS   3300000UL
#define HEARTBEAT_INTERVAL_MS  180000UL   // 3 minutes

// Backend Payments URL
#define BACKEND_API_URL    "https://freshpod-backend-324161304253.us-central1.run.app/api/payment/create"
#define QR_TIMEOUT_MS      180000 // 3 minutes timeout for scanning QR code

// ── MQTT / RAZORPAY CONFIG ────────────────────
#define MQTT_SERVER     "broker.hivemq.com" 
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "ESP32_Client_" MACHINE_ID
#define MQTT_TOPIC      "freshpod_vending_2025/" MACHINE_ID
#define MQTT_QOS        1
#define MQTT_RETAIN     false

// ── Relay & IO pins ───────────────────────────
#define RELAY1      13   // Tissue Dispenser
#define RELAY2      12   // Door Lock
#define RELAY3       2   // Fogging
#define RELAY4       4   // UV Sterilization
#define RELAY5      18   // Thermal Sterilization
#define RELAY6      19   // Exhaust system
#define BUZZER      23
#define ONBOARD_LED 27

// DWIN Serial Configuration
#define DWIN_RX_PIN 16
#define DWIN_TX_PIN 17

// DFPlayer tracks
#define TRACK_WELCOME               1
#define TRACK_PAYMENT_DONE          2
#define TRACK_DOOR_UNLOCKED         3
#define TRACK_DOOR_CLOSE            4
#define TRACK_UV_STERILIZATION      5
#define TRACK_UV_USES               6
#define TRACK_DRY_FOG               7
#define TRACK_DRY_FOG_USES          8
#define TRACK_THERMAL_DRYING        9
#define TRACK_THERMAL_DRYING_USES  10
#define TRACK_SANITIZING           11
#define TRACK_SANITIZED            12
#define TRACK_TISSUE_DISPENSED     13
#define TRACK_FRESHNESS            14
#define TRACK_THANK_YOU            15
#define TRACK_VISIT_AGAIN          16
#define TRACK_1MIN_TIME_REMAINDER  17
#define TRACK_30SEC_TIME_REMAINDER 18

// ─────────────────────────────────────────────
//  SYSTEM STATE DEFINITIONS
// ─────────────────────────────────────────────
enum MachineState {
  STATE_WELCOME,
  STATE_REQUEST_PAYMENT,
  STATE_WAIT_FOR_PAYMENT,
  STATE_CLEANING
};

MachineState currentState = STATE_WELCOME;
unsigned long stateTimer = 0;
unsigned long lastPollingTime = 0;

String currentQrId = "";
String currentUpiIntent = "";
bool paymentSuccessReceived = false;
bool qrPrefetched = false;

// Global Objects
DFPlayerMini_Fast myMP3;
HardwareSerial    dwinSerialPort(2);
WiFiClientSecure  secureClient; // SSL client for HTTPS REST API calls
WiFiClient        espClient;    // TCP client for MQTT connection
PubSubClient      mqttClient(espClient);
String            lastPaymentId = "";

String        idToken        = "";
String        refreshToken   = "";
unsigned long tokenFetchedAt = 0;

WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET, 60000);
HTTPClient httpClient;

// Forward Declarations
void startCleaningProcess();
void drawQRCode(const char *text);
bool requestNewPayment();
void pollPaymentStatus();
void connectWiFi();
void dgusShowLoadingIndicator();
void reconnectMQTT();
void reportHeartbeat();
void logTapToFirebase();
void checkAndPerformOTA();

// ─────────────────────────────────────────────
//  ONBOARD LED PATTERNS
// ─────────────────────────────────────────────
void ledOff() { digitalWrite(ONBOARD_LED, LOW);  }
void ledOn()  { digitalWrite(ONBOARD_LED, HIGH); }

void ledSlowBlink(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(ONBOARD_LED, HIGH); delay(600);
    digitalWrite(ONBOARD_LED, LOW);  delay(600);
  }
}
void ledDoubleBlink(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(ONBOARD_LED, HIGH); delay(150);
    digitalWrite(ONBOARD_LED, LOW);  delay(100);
    digitalWrite(ONBOARD_LED, HIGH); delay(150);
    digitalWrite(ONBOARD_LED, LOW);  delay(500);
  }
}
void ledTripleBlink(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(ONBOARD_LED, HIGH); delay(100);
    digitalWrite(ONBOARD_LED, LOW);  delay(100);
    digitalWrite(ONBOARD_LED, HIGH); delay(100);
    digitalWrite(ONBOARD_LED, LOW);  delay(100);
    digitalWrite(ONBOARD_LED, HIGH); delay(100);
    digitalWrite(ONBOARD_LED, LOW);  delay(600);
  }
}
void ledOTASuccess() {
  for (int i = 0; i < 15; i++) {
    digitalWrite(ONBOARD_LED, HIGH); delay(60);
    digitalWrite(ONBOARD_LED, LOW);  delay(60);
  }
}
void ledBootComplete() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(ONBOARD_LED, HIGH); delay(120);
    digitalWrite(ONBOARD_LED, LOW);  delay(120);
  }
}
void ledSOS() {
  for (int r = 0; r < 5; r++) {
    for (int i=0;i<3;i++){ledOn();delay(200);ledOff();delay(200);}
    for (int i=0;i<3;i++){ledOn();delay(600);ledOff();delay(200);}
    for (int i=0;i<3;i++){ledOn();delay(200);ledOff();delay(200);}
    delay(1000);
  }
}

// ─────────────────────────────────────────────
//  JSON EXTRACT
// ─────────────────────────────────────────────
String jsonExtract(const String& json, const String& key) {
  String s1 = "\"" + key + "\": \"";
  String s2 = "\"" + key + "\":\"";
  int start = json.indexOf(s1);
  int plen  = s1.length();
  if (start == -1) { start = json.indexOf(s2); plen = s2.length(); }
  if (start == -1) return "";
  start += plen;
  int end = json.indexOf("\"", start);
  if (end == -1) return "";
  return json.substring(start, end);
}

// ─────────────────────────────────────────────
//  SEMVER COMPARE
// ─────────────────────────────────────────────
bool isNewer(const String& current, const String& server) {
  int cMaj=0,cMin=0,cPat=0,sMaj=0,sMin=0,sPat=0;
  sscanf(current.c_str(), "%d.%d.%d", &cMaj, &cMin, &cPat);
  sscanf(server.c_str(),  "%d.%d.%d", &sMaj, &sMin, &sPat);
  if (sMaj != cMaj) return sMaj > cMaj;
  if (sMin != cMin) return sMin > cMin;
  return sPat > cPat;
}

// ─────────────────────────────────────────────
//  FIREBASE AUTH
// ─────────────────────────────────────────────
bool firebaseSignIn() {
  HTTPClient http;
  http.begin("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=" FIREBASE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{\"returnSecureToken\":true}");
  if (code != 200) { http.end(); return false; }
  String response = http.getString(); http.end();
  idToken      = jsonExtract(response, "idToken");
  refreshToken = jsonExtract(response, "refreshToken");
  if (idToken == "") return false;
  tokenFetchedAt = millis();
  return true;
}

bool firebaseRefreshToken() {
  if (refreshToken == "") return firebaseSignIn();
  HTTPClient http;
  http.begin("https://securetoken.googleapis.com/v1/token?key=" FIREBASE_API_KEY);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("grant_type=refresh_token&refresh_token=" + refreshToken);
  if (code != 200) { http.end(); return firebaseSignIn(); }
  String r = http.getString(); http.end();
  String s = "\"id_token\":\"";
  int i = r.indexOf(s); if (i != -1) { i += s.length(); idToken = r.substring(i, r.indexOf("\"",i)); }
  s = "\"refresh_token\":\"";
  i = r.indexOf(s); if (i != -1) { i += s.length(); refreshToken = r.substring(i, r.indexOf("\"",i)); }
  tokenFetchedAt = millis();
  return true;
}

bool ensureValidToken() {
  if (idToken == "") return firebaseSignIn();
  if (millis() - tokenFetchedAt >= TOKEN_REFRESH_MS) return firebaseRefreshToken();
  return true;
}

// ─────────────────────────────────────────────
//  NTP
// ─────────────────────────────────────────────
String getDateString() {
  timeClient.update();
  time_t raw = timeClient.getEpochTime();
  struct tm* t = gmtime(&raw);
  char buf[11]; strftime(buf, sizeof(buf), "%Y-%m-%d", t);
  return String(buf);
}

String getTimestampString() {
  timeClient.update();
  time_t raw = timeClient.getEpochTime();
  struct tm* t = gmtime(&raw);
  char buf[20]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
  return String(buf);
}

// ─────────────────────────────────────────────
//  FIREBASE REST
// ─────────────────────────────────────────────
int firebaseGetInt(const String& path) {
  if (!ensureValidToken()) return -1;
  HTTPClient http;
  http.begin("https://" + String(FIREBASE_HOST) + path + ".json?auth=" + idToken);
  int code = http.GET();
  if (code != 200) { http.end(); return -1; }
  String body = http.getString(); http.end(); body.trim();
  if (body == "null" || body == "") return 0;
  return body.toInt();
}

bool firebaseSetInt(const String& path, int value) {
  if (!ensureValidToken()) return false;
  HTTPClient http;
  http.begin("https://" + String(FIREBASE_HOST) + path + ".json?auth=" + idToken);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(String(value)); http.end();
  return (code == 200);
}

bool firebaseSetString(const String& path, const String& value) {
  if (!ensureValidToken()) return false;
  HTTPClient http;
  http.begin("https://" + String(FIREBASE_HOST) + path + ".json?auth=" + idToken);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT("\"" + value + "\""); http.end();
  return (code == 200);
}

void logTapToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  String path = "/machines/" MACHINE_ID "/logs/" + getDateString() + "/tapCount";
  int current = firebaseGetInt(path);
  if (current < 0) return;
  firebaseSetInt(path, current + 1);
}

// ─────────────────────────────────────────────
//  OTA FEEDBACK
// ─────────────────────────────────────────────
void reportOTASuccessToFirebase(const String& newVersion) {
  if (WiFi.status() != WL_CONNECTED) return;
  String base = "/machines/" MACHINE_ID "/ota";
  firebaseSetString(base + "/currentVersion", newVersion);
  firebaseSetString(base + "/lastUpdated",    getTimestampString());
  firebaseSetString(base + "/status",         "updated");
}

// ─────────────────────────────────────────────
//  HEARTBEAT — online/offline detection
// ─────────────────────────────────────────────
void reportHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  String base = "/machines/" MACHINE_ID;
  firebaseSetString(base + "/status",        "online");
  firebaseSetString(base + "/lastHeartbeat", getTimestampString());
}

// ─────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("[WIFI] Connecting to SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 25) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected. IP Address: " + WiFi.localIP().toString());
    digitalWrite(BUZZER, HIGH); delay(100); digitalWrite(BUZZER, LOW);
    return;
  }

  // Fallback to WiFiManager portal if ssid connection fails
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setSaveConnect(true);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);
  wm.setAPClientCheck(true);
  wm.startConfigPortal("Freshpod_Setup", ADMIN_AP_PASSWORD);
}

// ─────────────────────────────────────────────
//  OTA — check server
// ─────────────────────────────────────────────
static bool          otaLedState  = false;
static unsigned long otaLastBlink = 0;

void otaBlinkTick(int interval = 200) {
  unsigned long now = millis();
  if (now - otaLastBlink >= (unsigned long)interval) {
    otaLedState = !otaLedState;
    digitalWrite(ONBOARD_LED, otaLedState ? HIGH : LOW);
    digitalWrite(RELAY4,      otaLedState ? LOW  : HIGH);
    otaLastBlink = now;
  }
}

void otaBlinkStop() {
  digitalWrite(ONBOARD_LED, LOW);
  digitalWrite(RELAY4,      LOW);
  otaLedState = false;
}

String checkOTAUpdate(String& newVersionOut) {
  otaLastBlink = millis();
  otaLedState  = false;
  newVersionOut = "";

  if (WiFi.status() != WL_CONNECTED) { otaBlinkStop(); return ""; }

  HTTPClient http;
  http.begin(String(OTA_BASE_URL) + String(MACHINE_ID));
  http.setTimeout(15000);
  otaBlinkTick(200);
  int code = http.GET();
  otaBlinkTick(200);

  if (code != HTTP_CODE_OK) {
    http.end(); otaBlinkStop(); ledTripleBlink(2);
    return "";
  }

  String body = http.getString(); http.end();
  String serverVer   = jsonExtract(body, "version");
  String firmwareUrl = jsonExtract(body, "url");

  if (serverVer == "") {
    otaBlinkStop(); ledTripleBlink(2);
    return "";
  }

  if (!isNewer(CURRENT_VERSION_STR, serverVer)) {
    otaBlinkStop();
    ledOn(); delay(300); ledOff();
    return "";
  }

  newVersionOut = serverVer;
  return firmwareUrl;
}

// ─────────────────────────────────────────────
//  OTA — download & flash
// ─────────────────────────────────────────────
void performOTA(const String& firmwareUrl, const String& newVersion) {
  digitalWrite(RELAY1, LOW); digitalWrite(RELAY2, LOW);
  digitalWrite(RELAY3, LOW); digitalWrite(RELAY4, LOW);
  digitalWrite(RELAY5, LOW); digitalWrite(RELAY6, LOW);

  HTTPClient http;
  http.begin(firmwareUrl);
  http.setTimeout(60000);
  otaBlinkTick(200);
  int httpCode = http.GET();
  otaBlinkTick(200);

  if (httpCode != HTTP_CODE_OK) {
    http.end(); otaBlinkStop(); ledTripleBlink(5);
    digitalWrite(RELAY2, HIGH); return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    http.end(); otaBlinkStop(); ledTripleBlink(5);
    digitalWrite(RELAY2, HIGH); return;
  }

  if (!Update.begin(contentLength)) {
    http.end(); otaBlinkStop(); ledTripleBlink(5);
    digitalWrite(RELAY2, HIGH); return;
  }

  WiFiClient* stream  = http.getStreamPtr();
  size_t      written = 0;
  uint8_t     buf[512];

  while (http.connected() && written < (size_t)contentLength) {
    int pct      = (int)((written * 100UL) / (size_t)contentLength);
    int interval = (pct < 34) ? 300 : (pct < 67) ? 150 : 60;
    otaBlinkTick(interval);

    size_t available = stream->available();
    if (available) {
      size_t toRead    = min(available, sizeof(buf));
      size_t bytesRead = stream->readBytes(buf, toRead);
      if (Update.write(buf, bytesRead) != bytesRead) {
        Update.abort(); http.end();
        otaBlinkStop(); ledTripleBlink(5);
        digitalWrite(RELAY2, HIGH); return;
      }
      written += bytesRead;
    }
    delay(1);
  }

  http.end();

  if (written != (size_t)contentLength) {
    Update.abort();
    otaBlinkStop(); ledTripleBlink(5);
    digitalWrite(RELAY2, HIGH); return;
  }

  if (Update.end(true)) {
    for (int i = 0; i < 20; i++) {
      digitalWrite(ONBOARD_LED, (i % 2 == 0) ? HIGH : LOW);
      digitalWrite(RELAY4,      (i % 2 == 0) ? LOW  : HIGH);
      delay(60);
    }
    otaBlinkStop();

    timeClient.update();
    reportOTASuccessToFirebase(newVersion);

    digitalWrite(BUZZER, HIGH); delay(1500);
    digitalWrite(BUZZER, LOW);
    delay(3000);
    ESP.restart();
  } else {
    otaBlinkStop(); ledTripleBlink(5);
    digitalWrite(RELAY2, HIGH);
  }
}

// ─────────────────────────────────────────────
//  OTA — entry point
// ─────────────────────────────────────────────
void checkAndPerformOTA() {
  if (WiFi.status() != WL_CONNECTED) return;
  String newVersion = "";
  String url = checkOTAUpdate(newVersion);
  if (url != "") performOTA(url, newVersion);
}

// ─────────────────────────────────────────────
//  Draw the QR Code using manual geometric spans
// ─────────────────────────────────────────────
void drawQRCode(const char *text) {
  if (text == NULL || strlen(text) == 0) {
    Serial.println("[ERROR] Empty QR text");
    return;
  }

  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(4)];

  Serial.print("[QR] Drawing manual QR code for: ");
  Serial.println(text);

  int result = qrcode_initText(&qrcode, qrcodeData, 4, ECC_LOW, text);
  if (result != 0) {
    Serial.println("[ERROR] QR generation failed.");
    return;
  }

  // Draw white background block first
  dgusClearQrArea();
  delay(80);

  // Math placement parameters
  uint16_t startX = 329;
  uint16_t startY = 199;
  uint16_t moduleSize = 5;

  static DGUSRect spansBuffer[300];
  uint16_t spanCount = 0;

  for (uint8_t y = 0; y < qrcode.size; y++) {
    int runStart = -1;
    for (uint8_t x = 0; x < qrcode.size; x++) {
      bool isBlack = qrcode_getModule(&qrcode, x, y);
      if (isBlack) {
        if (runStart == -1) runStart = x;
      } else {
        if (runStart != -1) {
          if (spanCount < 300) {
            uint16_t xs = startX + runStart * moduleSize;
            uint16_t ys = startY + y * moduleSize;
            uint16_t xe = startX + x * moduleSize - 1;
            uint16_t ye = ys + moduleSize - 1;
            spansBuffer[spanCount++] = {xs, ys, xe, ye, COLOR_BLACK};
          }
          runStart = -1;
        }
      }
    }
    if (runStart != -1) {
      if (spanCount < 300) {
        uint16_t xs = startX + runStart * moduleSize;
        uint16_t ys = startY + y * moduleSize;
        uint16_t xe = startX + qrcode.size * moduleSize - 1;
        uint16_t ye = ys + moduleSize - 1;
        spansBuffer[spanCount++] = {xs, ys, xe, ye, COLOR_BLACK};
      }
    }
  }

  dgusDrawRects(spansBuffer, spanCount);
  Serial.println("[QR] DWIN manual rendering completed.");
}

// ─────────────────────────────────────────────
//  DWIN Loading Progress Bar
// ─────────────────────────────────────────────
void dgusShowLoadingIndicator() {
  Serial.println("[DWIN] Drawing loading progress indicator...");
  dgusClearQrArea();
  dgusDrawFilledRect(325, 240, 475, 260, COLOR_BLACK);
  dgusDrawFilledRect(327, 242, 473, 258, COLOR_WHITE);
  dgusDrawFilledRect(332, 245, 400, 255, 0x3186); // pastel blue color
}

// ─────────────────────────────────────────────
//  Call backend REST API to create dynamic QR
// ─────────────────────────────────────────────
bool requestNewPayment() {
  String url = BACKEND_API_URL;
  bool isHttps = url.startsWith("https://");

  bool beginSuccess = false;
  if (isHttps) {
    beginSuccess = httpClient.begin(secureClient, url);
  } else {
    WiFiClient client;
    beginSuccess = httpClient.begin(client, url);
  }

  if (!beginSuccess) {
    Serial.println("[ERROR] HTTP begin failed.");
    return false;
  }

  httpClient.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["machine_id"] = MACHINE_ID;

  String requestBody;
  serializeJson(doc, requestBody);

  Serial.print("[HTTP] Requesting QR details: ");
  Serial.println(url);

  int httpResponseCode = httpClient.POST(requestBody);
  bool success = false;

  if (httpResponseCode == 200) {
    String responseString = httpClient.getString();
    Serial.println("[HTTP] Response: " + responseString);
    DynamicJsonDocument respDoc(1024);
    DeserializationError error = deserializeJson(respDoc, responseString);

    if (!error) {
      currentQrId = respDoc["qr_id"].as<String>();
      currentUpiIntent = respDoc["upi_intent"].as<String>();
      Serial.println("[HTTP] Success! QR ID = " + currentQrId);
      success = true;
    } else {
      Serial.println("[ERROR] JSON Parsing failed.");
    }
  } else {
    Serial.print("[ERROR] HTTP request failed with code: ");
    Serial.println(httpResponseCode);
  }

  httpClient.end();
  return success;
}

// ─────────────────────────────────────────────
//  Poll payment status from API directly
// ─────────────────────────────────────────────
void pollPaymentStatus() {
  if (currentQrId == "") return;

  String statusUrl = BACKEND_API_URL;
  statusUrl.replace("/create", "/status");
  statusUrl += "?qr_id=" + currentQrId;

  bool isHttps = statusUrl.startsWith("https://");
  bool beginSuccess = false;

  if (isHttps) {
    beginSuccess = httpClient.begin(secureClient, statusUrl);
  } else {
    WiFiClient client;
    beginSuccess = httpClient.begin(client, statusUrl);
  }

  if (!beginSuccess) {
    Serial.println("[ERROR] Poll begin failed.");
    return;
  }

  httpClient.addHeader("Connection", "keep-alive");

  Serial.print("[HTTP] Polling status: ");
  Serial.println(statusUrl);

  int httpResponseCode = httpClient.GET();

  if (httpResponseCode == 200) {
    String responseString = httpClient.getString();
    Serial.println("[HTTP] Status Response: " + responseString);
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, responseString);

    if (!error) {
      const char *status = doc["status"];
      if (status && String(status) == "paid") {
        Serial.println("[SUCCESS] Paid via API Polling!");
        paymentSuccessReceived = true;
      }
    }
  }

  httpClient.end();
}

// ─────────────────────────────────────────────
//  MQTT — reconnect
// ─────────────────────────────────────────────
void reconnectMQTT() {
  int retryCount = 0;
  const int maxRetries = 3;

  while (!mqttClient.connected() && retryCount < maxRetries) {
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      bool subResult = mqttClient.subscribe(MQTT_TOPIC, MQTT_QOS);
      delay(100);
      if (subResult) {
        digitalWrite(BUZZER, HIGH); delay(500);
        digitalWrite(BUZZER, LOW);  delay(100);
      } else {
        mqttClient.disconnect();
      }
    }
    retryCount++;
    if (!mqttClient.connected() && retryCount < maxRetries) delay(5000);
  }
}

// ─────────────────────────────────────────────
//  MQTT — message callback
// ─────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, message);
  if (error) return;

  const char* command = doc["command"];
  const char* action = doc["action"];
  const char* transaction_id = doc["transaction_id"];

  // 1. Handle Ping diagnostic request
  if ((command && strcmp(command, "ping") == 0) || (action && strcmp(action, "ping") == 0)) {
    String responseTopic = String("freshpod_vending_2025/") + MACHINE_ID + "/response";
    String statusMsg = "{\"status\":\"online\",\"rssi\":" + String(WiFi.RSSI()) + ",\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"version\":\"" + CURRENT_VERSION_STR + "\"}";
    mqttClient.publish(responseTopic.c_str(), statusMsg.c_str());
    return;
  }

  // 2. Handle Start signal trigger (Manual runs / Remote overrides)
  bool isStart = (command && strcmp(command, "start") == 0) || (action && strcmp(action, "START") == 0);
  if (isStart) {
    String txnId = transaction_id ? String(transaction_id) : String("TXN_MQTT_") + String(millis());
    if (txnId != lastPaymentId) {
      lastPaymentId = txnId;

      logTapToFirebase();

      // Interrupt polling loop state machine and jump to STATE_CLEANING
      paymentSuccessReceived = true;
      currentState = STATE_CLEANING;

      String responseTopic = String("freshpod_vending_2025/") + MACHINE_ID + "/response";
      
      // Notify backend: Dispense started
      String startAck = "{\"status\":\"started\",\"transaction_id\":\"" + txnId + "\"}";
      mqttClient.publish(responseTopic.c_str(), startAck.c_str());
    }
  }
}

// ─────────────────────────────────────────────
//  Dispensing Hardware Cycle Sequence
// ─────────────────────────────────────────────
void startCleaningProcess() {
  Serial.println("=== STARTING Dispensing HW CYCLE ===");
  dgusClearQrArea(); // Clear the screen QR code area instantly

  digitalWrite(BUZZER, HIGH); delay(500); digitalWrite(BUZZER, LOW); delay(250);
  digitalWrite(RELAY2, LOW); delay(500); // Door Unlock
  dgusShowPage(PAGE_PAYMENT_SUCCESS); delay(1000);
  myMP3.play(TRACK_PAYMENT_DONE); delay(7000);

  dgusShowPage(PAGE_DOOR_UNLOCKED); delay(1000);
  myMP3.play(TRACK_DOOR_UNLOCKED); delay(9000);

  dgusShowPage(PAGE_CLOSE_DOOR); delay(500);
  myMP3.play(TRACK_DOOR_CLOSE); delay(13000);

  digitalWrite(RELAY2, HIGH); delay(1000); // Closed/locked
  dgusShowPage(PAGE_DUST_REMOVAL); delay(1000);
  
  digitalWrite(RELAY4, HIGH); delay(1000); // UV Light ON
  digitalWrite(RELAY5, HIGH); delay(1000); // Thermal Drying ON
  digitalWrite(RELAY3, HIGH); delay(1000); // Fogger Pump ON
  myMP3.play(TRACK_UV_STERILIZATION); delay(8000);
  dgusShowPage(PAGE_UV_STERILIZATION); delay(1000);

  myMP3.play(TRACK_UV_USES); delay(21000);

  digitalWrite(RELAY5, LOW); delay(1000); // Thermal OFF
  dgusShowPage(PAGE_CLEANING_STARTED); delay(1000);
  myMP3.play(TRACK_DRY_FOG); delay(70000);

  myMP3.play(TRACK_DRY_FOG_USES); delay(58000);

  dgusShowPage(PAGE_SANITIZING); delay(1000);
  myMP3.play(TRACK_SANITIZING); delay(25000);

  digitalWrite(RELAY3, LOW); delay(500); // Fogger OFF
  digitalWrite(RELAY5, HIGH); delay(5000); // Thermal ON
  dgusShowPage(PAGE_DRY_HELMET); delay(500);
  myMP3.play(TRACK_THERMAL_DRYING); delay(16000);

  myMP3.play(TRACK_THERMAL_DRYING_USES); delay(26000);

  // Dispenser & Exhaust Sequence
  digitalWrite(RELAY4, LOW); // UV Light OFF
  digitalWrite(RELAY1, HIGH); delay(15000); // Exhaust system ON
  myMP3.play(TRACK_30SEC_TIME_REMAINDER); delay(10000);
  digitalWrite(RELAY6, HIGH); delay(3000); // Tissue Dispenser ON

  dgusShowPage(PAGE_HELMET_DISINFECTION); delay(1000);
  digitalWrite(RELAY5, LOW); delay(1000); // Thermal drying OFF
  digitalWrite(RELAY1, LOW); delay(500); // Exhaust system OFF
  digitalWrite(RELAY6, LOW); delay(1000); // Tissue dispenser OFF
  
  dgusShowPage(PAGE_TAKE_HELMET); delay(2000);
  digitalWrite(RELAY4, LOW); delay(1000);
  digitalWrite(RELAY2, LOW); delay(5000); // Unlock Door
  myMP3.play(TRACK_SANITIZED); delay(10000);

  dgusShowPage(PAGE_CLOSE_DOOR); delay(3500);
  digitalWrite(RELAY2, HIGH); delay(1000); // Lock door back up
  myMP3.play(TRACK_FRESHNESS); delay(12000);

  dgusShowPage(PAGE_THANK_YOU); delay(1000);
  myMP3.play(TRACK_THANK_YOU);

  // Prefetch the next payment QR link in the background during the static screen display
  Serial.println("[PREFETCH] Prefetching next payment link from backend...");
  if (requestNewPayment()) {
    qrPrefetched = true;
    Serial.println("[PREFETCH] Success! Next QR code loaded.");
  } else {
    qrPrefetched = false;
    Serial.println("[PREFETCH] Warning: prefetch failed.");
  }

  delay(7000);
  Serial.println("=== CLEANING CYCLE COMPLETE ===");
  myMP3.play(TRACK_VISIT_AGAIN);
  delay(14000);

  // Reset all relays to safe default
  digitalWrite(RELAY1, LOW); delay(1000);
  digitalWrite(RELAY2, HIGH); delay(1000);
  digitalWrite(RELAY3, LOW); delay(1000);
  digitalWrite(RELAY4, LOW); delay(1000);
  digitalWrite(RELAY5, LOW); delay(1000);
  digitalWrite(RELAY6, LOW); delay(1000);

  // Notify backend: Dispense completed
  String responseTopic = String("freshpod_vending_2025/") + MACHINE_ID + "/response";
  String completeAck = "{\"status\":\"completed\",\"transaction_id\":\"" + lastPaymentId + "\"}";
  mqttClient.publish(responseTopic.c_str(), completeAck.c_str());
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  // Initialize DWIN display and switch to welcome page immediately
  dgusInit(dwinSerialPort, DWIN_RX_PIN, DWIN_TX_PIN, 115200);
  delay(50);
  dgusShowPage(PAGE_WELCOME);
  delay(50);
  dgusClearQrArea();
  delay(50);

  Serial.begin(9600);
  Serial.println("\n--- Freshpod ESP32 Hybrid Boot Starting ---");

  secureClient.setInsecure();

  // Initialize DFPlayer
  if (!myMP3.begin(Serial)) {
    Serial.println("DFPlayer failed to start! Halting...");
    ledSOS();
    while (1);
  }
  myMP3.volume(80);

  // Initialize relays
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT);
  pinMode(RELAY4, OUTPUT);
  pinMode(RELAY5, OUTPUT);
  pinMode(RELAY6, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(ONBOARD_LED, OUTPUT);

  // Initial relay states
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, HIGH); // Closed/locked by default
  digitalWrite(RELAY3, LOW);
  digitalWrite(RELAY4, LOW);
  digitalWrite(RELAY5, LOW);
  digitalWrite(RELAY6, LOW);
  digitalWrite(BUZZER, LOW);
  ledOff();

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    timeClient.update();
    for (int i = 0; i < 3; i++) {
      if (firebaseSignIn()) break;
      delay(2000);
    }
    // Check and execute OTA updates
    checkAndPerformOTA();
  }

  myMP3.play(TRACK_WELCOME);
  delay(4000);

  // Setup MQTT telemetry channels
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);
  mqttClient.setKeepAlive(60);
  reconnectMQTT();

  // Display initialization sequences
  dgusShowPage(PAGE_WELCOME);
  stateTimer = millis();
  currentState = STATE_WELCOME;
  ledBootComplete();

  reportHeartbeat();
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
void loop() {
  // Keep WiFi active
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // MQTT client tick
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Non-blocking Heartbeat
  static unsigned long lastHeartbeatMs = 0;
  if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    reportHeartbeat();
    lastHeartbeatMs = millis();
  }

  // State Machine logic
  switch (currentState) {
    case STATE_WELCOME:
      if (millis() - stateTimer >= 5000) {
        Serial.println("Transitioning to QR page, requesting payment details...");
        currentState = STATE_REQUEST_PAYMENT;
      }
      break;

    case STATE_REQUEST_PAYMENT:
      dgusShowPage(PAGE_QR_CODE);
      dgusClearQrArea();

      if (qrPrefetched) {
        Serial.println("[PREFETCH] Using pre-fetched payment QR code intent.");
        drawQRCode(currentUpiIntent.c_str());
        qrPrefetched = false;
        paymentSuccessReceived = false;
        stateTimer = millis();
        lastPollingTime = millis();
        currentState = STATE_WAIT_FOR_PAYMENT;
      } else if (requestNewPayment()) {
        Serial.println("QR generated successfully. Rendering...");
        drawQRCode(currentUpiIntent.c_str());
        paymentSuccessReceived = false;
        stateTimer = millis();
        lastPollingTime = millis();
        currentState = STATE_WAIT_FOR_PAYMENT;
      } else {
        Serial.println("[WARNING] Payment creation failed. Retrying in 5 seconds...");
        dgusShowLoadingIndicator();
        delay(5000);
        stateTimer = millis();
      }
      break;

    case STATE_WAIT_FOR_PAYMENT:
      if (paymentSuccessReceived) {
        Serial.println("Payment SUCCESS detected. Transitioning to cleaning sequence...");
        currentState = STATE_CLEANING;
        break;
      }

      // Check timeout (back to welcome if not scanned after QR_TIMEOUT_MS)
      if (millis() - stateTimer >= QR_TIMEOUT_MS) {
        Serial.println("[TIMEOUT] Payment window expired. Returning to Welcome screen.");
        dgusShowPage(PAGE_WELCOME);
        myMP3.play(TRACK_WELCOME);
        stateTimer = millis();
        currentState = STATE_WELCOME;
        break;
      }

      // Poll status via API every 2 seconds
      if (millis() - lastPollingTime >= 2000) {
        lastPollingTime = millis();
        pollPaymentStatus();
      }
      break;

    case STATE_CLEANING:
      startCleaningProcess();

      // Transition based on prefetch success
      stateTimer = millis();
      if (qrPrefetched) {
        Serial.println("[LOOP] Drawing pre-fetched QR code and entering STATE_WAIT_FOR_PAYMENT.");
        dgusShowPage(PAGE_QR_CODE);
        dgusClearQrArea();
        drawQRCode(currentUpiIntent.c_str());
        qrPrefetched = false;
        paymentSuccessReceived = false;
        lastPollingTime = millis();
        currentState = STATE_WAIT_FOR_PAYMENT;
      } else {
        Serial.println("[LOOP] QR prefetch failed. Fallback to requesting payment synchronously.");
        currentState = STATE_REQUEST_PAYMENT;
      }
      break;
  }
}
