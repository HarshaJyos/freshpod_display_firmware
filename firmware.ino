#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <HardwareSerial.h>
#include <DFPlayerMini_Fast.h>
#include <NTPClient.h>
#include <WiFiUdp.h> 
#include <PubSubClient.h>
#include <ArduinoJson.h>

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

// ── MQTT / RAZORPAY CONFIG ────────────────────
#define MQTT_SERVER     "broker.hivemq.com" // Swapped EMQX for HiveMQ to match backend env
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

typedef struct {
  uint8_t*    command;
  size_t      length;
  const char* description;
} DwinCommand;

DFPlayerMini_Fast myMP3;
HardwareSerial    mySerial(1);

WiFiClient    espClient;
PubSubClient  mqttClient(espClient);
String        lastPaymentId = "";

String        idToken        = "";
String        refreshToken   = "";
unsigned long tokenFetchedAt = 0;

WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET, 60000);

// DWIN commands
uint8_t cmdPageQR[]                 = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x00};
uint8_t cmdPageCleaningStarted[]    = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x01};
uint8_t cmdPageUVSterilization[]    = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x02};
uint8_t cmdPageDoorUnlocked[]       = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x03};
uint8_t cmdPageHelmetDisinfection[] = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x04};
uint8_t cmdPageTakeHelmet[]         = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x05};
uint8_t cmdPagePaymentSuccess[]     = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x06};
uint8_t cmdPageCloseDoor[]          = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x00,0x00,0x07};
uint8_t cmdPageThankYou[]           = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x08};
uint8_t cmdPageDustRemoval[]        = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x09};
uint8_t cmdPageDryHelmet[]          = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x0A};
uint8_t cmdPageSanitizing[]         = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x0B};
uint8_t cmdPageWelcome[]            = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x0C};

DwinCommand dwinCommands[] = {
  {cmdPageQR,                 sizeof(cmdPageQR),                 "QR Code Page"},
  {cmdPageCleaningStarted,    sizeof(cmdPageCleaningStarted),    "Cleaning Started"},
  {cmdPageUVSterilization,    sizeof(cmdPageUVSterilization),    "UV Sterilization"},
  {cmdPageDoorUnlocked,       sizeof(cmdPageDoorUnlocked),       "Door Unlocked"},
  {cmdPageHelmetDisinfection, sizeof(cmdPageHelmetDisinfection), "Helmet Disinfection"},
  {cmdPageTakeHelmet,         sizeof(cmdPageTakeHelmet),         "Take Helmet"},
  {cmdPagePaymentSuccess,     sizeof(cmdPagePaymentSuccess),     "Payment Success"},
  {cmdPageCloseDoor,          sizeof(cmdPageCloseDoor),          "Close Door"},
  {cmdPageThankYou,           sizeof(cmdPageThankYou),           "Thank You"},
  {cmdPageDustRemoval,        sizeof(cmdPageDustRemoval),        "Dust Removal"},
  {cmdPageDryHelmet,          sizeof(cmdPageDryHelmet),          "Dry Helmet"},
  {cmdPageSanitizing,         sizeof(cmdPageSanitizing),         "Sanitizing"},
  {cmdPageWelcome,            sizeof(cmdPageWelcome),            "Welcome"}
};

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
//  DWIN
// ─────────────────────────────────────────────
void sendDwinCommand(uint8_t pageIndex) {
  if (pageIndex < sizeof(dwinCommands) / sizeof(dwinCommands[0])) {
    mySerial.write(dwinCommands[pageIndex].command, dwinCommands[pageIndex].length);
    mySerial.flush();
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
  ledSlowBlink(2);
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  delay(500);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    ledOn(); delay(400); ledOff();
    return;
  }
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER, HIGH); delay(200);
    digitalWrite(BUZZER, LOW);  delay(200);
  }
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setSaveConnect(true);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);
  wm.setAPClientCheck(true);
  wm.startConfigPortal("Freshpod_Setup", ADMIN_AP_PASSWORD);
  if (WiFi.isConnected()) {
    ledOn(); delay(400); ledOff();
  } else {
    for (int i = 0; i < 2; i++) {
      digitalWrite(BUZZER, HIGH); delay(600);
      digitalWrite(BUZZER, LOW);  delay(300);
    }
  }
}

bool reconnectWiFi() {
  if (WiFi.isConnected()) return true;
  WiFi.reconnect();
  int retry = 0;
  while (!WiFi.isConnected() && retry < 15) {
    delay(500); retry++;
  }
  return WiFi.isConnected();
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

  if (!WiFi.isConnected()) { otaBlinkStop(); return ""; }

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
  if (!reconnectWiFi()) return;
  String newVersion = "";
  String url = checkOTAUpdate(newVersion);
  if (url != "") performOTA(url, newVersion);
}

// ─────────────────────────────────────────────
//  CLEANING SEQUENCE
// ─────────────────────────────────────────────
void startCleaningProcess() {
  digitalWrite(BUZZER, HIGH); delay(500); digitalWrite(BUZZER, LOW); delay(250);
  digitalWrite(RELAY2, LOW); delay(500);
  sendDwinCommand(6); delay(1000);
  myMP3.play(2); delay(7000);

  sendDwinCommand(3); delay(1000);
  myMP3.play(3); delay(9000);

  sendDwinCommand(7); delay(500);
  myMP3.play(4); delay(13000);

  digitalWrite(RELAY2, HIGH); delay(1000);
  sendDwinCommand(9); delay(1000);

  // ── PHASE A: UV ON continuously ──
  digitalWrite(RELAY4, HIGH);
  digitalWrite(RELAY5, HIGH); delay(1000);
  digitalWrite(RELAY3, HIGH); delay(1000);
  myMP3.play(5);
  delay(8000);

  sendDwinCommand(2); delay(1000);
  myMP3.play(6);
  delay(21000);

  digitalWrite(RELAY5, LOW); delay(1000);
  sendDwinCommand(1); delay(1000);

  myMP3.play(7);
  delay(70000);

  myMP3.play(8);
  delay(58000);

  sendDwinCommand(11); delay(1000);
  myMP3.play(11);
  delay(25000);

  // ── PHASE B: Thermal drying — UV stays ON ──
  digitalWrite(RELAY3, LOW); delay(500);
  digitalWrite(RELAY5, HIGH);

  sendDwinCommand(10); delay(500);
  myMP3.play(9);
  delay(16000);

  myMP3.play(10);
  delay(26000);

  // ── UV OFF — tissue dispense, exhaust, thank-you ──
  digitalWrite(RELAY4, LOW);

  digitalWrite(RELAY1, HIGH); delay(15000);
  myMP3.play(18); delay(10000);
  digitalWrite(RELAY6, HIGH); delay(3000);
  sendDwinCommand(4); delay(1000);
  digitalWrite(RELAY5, LOW); delay(1000);
  digitalWrite(RELAY1, LOW); delay(500);
  digitalWrite(RELAY6, LOW); delay(1000);
  sendDwinCommand(5); delay(2000);
  digitalWrite(RELAY4, LOW); delay(1000);
  digitalWrite(RELAY2, LOW); delay(5000);
  myMP3.play(12); delay(10000);

  sendDwinCommand(7); delay(3500);
  digitalWrite(RELAY2, HIGH); delay(1000);
  myMP3.play(14); delay(12000);

  sendDwinCommand(8); delay(1000);
  myMP3.play(15); delay(7000);

  myMP3.play(16); delay(14000);
  sendDwinCommand(0);

  // ── Safety: ensure all relays are off ──
  digitalWrite(RELAY1, LOW); delay(1000);
  digitalWrite(RELAY2, HIGH); delay(1000);
  digitalWrite(RELAY3, LOW); delay(1000);
  digitalWrite(RELAY4, LOW); delay(1000);
  digitalWrite(RELAY5, LOW); delay(1000);
  digitalWrite(RELAY6, LOW); delay(1000);
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

  if (!mqttClient.connected()) {
    ESP.restart();
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

  // 1. Handle Ping check telemetry request
  if ((command && strcmp(command, "ping") == 0) || (action && strcmp(action, "ping") == 0)) {
    String responseTopic = String("freshpod_vending_2025/") + MACHINE_ID + "/response";
    String statusMsg = "{\"status\":\"online\",\"rssi\":" + String(WiFi.RSSI()) + ",\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"version\":\"" + CURRENT_VERSION_STR + "\"}";
    mqttClient.publish(responseTopic.c_str(), statusMsg.c_str());
    return;
  }

  // 2. Handle Start telemetry request
  bool isStart = (command && strcmp(command, "start") == 0) || (action && strcmp(action, "START") == 0);
  if (isStart) {
    String txnId = transaction_id ? String(transaction_id) : String("TXN_MQTT_") + String(millis());
    if (txnId != lastPaymentId) {
      lastPaymentId = txnId;

      logTapToFirebase();

      String responseTopic = String("freshpod_vending_2025/") + MACHINE_ID + "/response";
      
      // Notify backend: Dispense started
      String startAck = "{\"status\":\"started\",\"transaction_id\":\"" + txnId + "\"}";
      mqttClient.publish(responseTopic.c_str(), startAck.c_str());

      startCleaningProcess();

      // Notify backend: Dispense finished successfully
      String completeAck = "{\"status\":\"completed\",\"transaction_id\":\"" + txnId + "\"}";
      mqttClient.publish(responseTopic.c_str(), completeAck.c_str());
    }
  }
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  delay(500);

  pinMode(ONBOARD_LED, OUTPUT); ledOff();
  pinMode(RELAY1, OUTPUT); pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT); pinMode(RELAY4, OUTPUT);
  pinMode(RELAY5, OUTPUT); pinMode(RELAY6, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, HIGH);  // Door locked by default
  digitalWrite(RELAY3, LOW);
  digitalWrite(RELAY4, LOW);
  digitalWrite(RELAY5, LOW);
  digitalWrite(RELAY6, LOW);

  mySerial.begin(115200, SERIAL_8N1, 16, 17);

  if (!myMP3.begin(Serial)) {
    ledSOS();
    while (1);
  }
  myMP3.volume(80);

  connectWiFi();

  if (WiFi.isConnected()) {
    timeClient.begin();
    timeClient.update();
    for (int i = 0; i < 3; i++) {
      if (firebaseSignIn()) break;
      delay(2000);
    }
    checkAndPerformOTA();
  }

  myMP3.play(TRACK_WELCOME);
  delay(4000);

  if (WiFi.isConnected()) {
    digitalWrite(BUZZER, HIGH); delay(2500);
    digitalWrite(BUZZER, LOW);  delay(2000);
  }

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);
  mqttClient.setKeepAlive(60);
  reconnectMQTT();

  sendDwinCommand(12);
  delay(5000);
  sendDwinCommand(0);

  ledBootComplete();
  reportHeartbeat();
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
static unsigned long lastHeartbeatMs = 0;

void loop() {
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    reportHeartbeat();
    lastHeartbeatMs = millis();
  }
}
