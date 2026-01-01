#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <time.h>
#include "DHT.h"
#include <PubSubClient.h>

// -------------------- DHT SETUP --------------------
#define DHTPIN 4         // GPIO4 = D2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// -------------------- WIFI SETUP -------------------
const char* ssid     = "DOffice"; // your WiFi SSID
const char* password = "kingandqueen"; // your WiFi password

ESP8266WebServer server(80);
WebSocketsServer webSocket(81);

// -------------------- MQTT (Mosquitto) -------------
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Public Mosquitto broker (free, no auth)
const char* MQTT_SERVER = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;

// Use a long, unique topic string (must match dashboard)
const char* MQTT_TOPIC  = "6d3647b111db22048d788f62b455eac1da5d3da9561cb8b7772959438eabbec3/data";  // << change, match in dashboard

// -------------------- FAN / LIGHT CONFIG -----------
// Pick unused GPIOs (examples below: D1 and D5)
const int FAN_PIN   = 5;   // D1
const int LIGHT_PIN = 14;  // D5

// Fan thresholds (°C)
float FAN_ON_TEMP_C  = 29.0f;  // fan turns ON when temp >= this
float FAN_OFF_TEMP_C = 27.0f;  // fan turns OFF when temp <= this

// Light thresholds (°C)
float LIGHT_ON_TEMP_C  = 10.0f; // light ON if temp <= this
float LIGHT_OFF_TEMP_C = 15.0f; // light OFF if temp >= this
const unsigned long LIGHT_ON_DURATION_MS = 3600000UL; // 1 hour

// -------------------- DATA RATE --------------------
// Single knob to control SAMPLE + LOG + PUBLISH rate
const unsigned long DATA_RATE_SECONDS = 60;               // seconds
const unsigned long DATA_RATE_MS      = DATA_RATE_SECONDS * 1000UL;
const unsigned long SAMPLES_PER_DAY   = 86400UL / DATA_RATE_SECONDS;

// -------------------- GLOBALS ----------------------
float tempC = 0.0;
float tempF = 0.0;
float hum   = 0.0;

unsigned long lastSampleTimeMs = 0;

// Offline logging state
unsigned long offlineSeconds   = 0;
unsigned long offlineLogCount  = 0;

// Online midnight reboot state
int lastOnlineDay = -1;

// Fan / Light state
bool fanOn   = false;
bool lightOn = false;
unsigned long lightOffDeadlineMs = 0; // millis() when light should auto-off

String lastFanChangeStr   = "N/A";
String lastLightChangeStr = "N/A";

// -------------------- FORWARD DECLS ----------------
void handleRoot();
void handleNotFound();
void handleHistory();
void handleReboot();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void sampleAndBroadcast();
void listFS();
void setupTime();
String getDateStr();
String getTimeStr();
String getDateTimeStr();
void logSample(float tempC, float tempF, float hum);
void reconnectMqtt();
void updateFanAndLight(float tempC, unsigned long nowMs);

void setup() {
  Serial.begin(115200);
  delay(10);
  Serial.println();
  Serial.println("=== DHT11 WebSocket + LittleFS + datalog.csv + MQTT (Mosquitto) + Fan/Light started ===");

  dht.begin();

  // Fan & Light pins
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, HIGH);
  fanOn = false;

  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, HIGH);
  lightOn = false;
  lightOffDeadlineMs = 0;

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed!");
  } else {
    Serial.println("LittleFS mounted OK.");

    // Clear datalog.csv on every boot/restart
    if (LittleFS.exists("/datalog.csv")) {
      Serial.println("Deleting existing /datalog.csv on boot");
      LittleFS.remove("/datalog.csv");
    }

    listFS();
  }

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    setupTime();  // NTP time sync + DST rules

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  } else {
    Serial.println("Failed to connect to WiFi. Will log in offline mode if needed.");
  }

  // HTTP routes
  server.on("/", handleRoot);
  server.on("/history", handleHistory);         // returns datalog.csv as JSON
  server.on("/reboot", HTTP_POST, handleReboot);// Reboot endpoint
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started on port 80.");

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81.");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  // MQTT loop if WiFi is up
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop();
  }

  sampleAndBroadcast();
}

// -------------------- TIME SETUP -------------------
void setupTime() {
  configTime(0, 0,
             "pool.ntp.org", "time.nist.gov", "time.google.com");

  // Melbourne DST auto-adjust
  setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
  tzset();

  Serial.print("Waiting for NTP time");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 8 * 3600 * 2 && retries < 40) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retries++;
  }
  Serial.println();

  if (now > 8 * 3600 * 2) {
    Serial.println("Time synchronized (with DST auto-adjust).");
  } else {
    Serial.println("Failed to sync time. Will fall back to offline logging.");
  }
}

String getDateStr() {
  time_t now = time(nullptr);
  if (now < 24 * 60 * 60) return "";
  struct tm *tm_info = localtime(&now);
  if (!tm_info) return "";
  char buf[11]; // YYYY-MM-DD
  strftime(buf, sizeof(buf), "%Y-%m-%d", tm_info);
  return String(buf);
}

String getTimeStr() {
  time_t now = time(nullptr);
  if (now < 24 * 60 * 60) return "";
  struct tm *tm_info = localtime(&now);
  if (!tm_info) return "";
  char buf[9]; // HH:MM:SS
  strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
  return String(buf);
}

// Full date-time string for fan/light changes
String getDateTimeStr() {
  time_t now = time(nullptr);
  if (now < 24 * 60 * 60) return "unknown";
  struct tm *tm_info = localtime(&now);
  if (!tm_info) return "unknown";
  char buf[20]; // YYYY-MM-DD HH:MM:SS
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
  return String(buf);
}

// -------------------- MQTT RECONNECT ---------------
void reconnectMqtt() {
  if (mqttClient.connected()) return;

  Serial.print("Attempting MQTT connection to ");
  Serial.print(MQTT_SERVER);
  Serial.print(" ... ");

  String clientId = "esp8266-";
  clientId += String(ESP.getChipId(), HEX);

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("connected.");
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" – will retry later.");
  }
}

// -------------------- HTTP HANDLERS ----------------
void handleRoot() {
  if (!LittleFS.exists("/index.html")) {
    Serial.println("ERROR: /index.html not found in LittleFS");
    server.send(500, "text/plain", "index.html not found in LittleFS");
    return;
  }

  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    Serial.println("ERROR: failed to open /index.html");
    server.send(500, "text/plain", "Failed to open index.html");
    return;
  }

  server.streamFile(file, "text/html");
  file.close();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  Serial.println("Reboot requested via /reboot");
  delay(200);
  ESP.restart();
}

void handleHistory() {
  Serial.println("HISTORY REQUEST /datalog.csv");

  if (!LittleFS.exists("/datalog.csv")) {
    server.send(200, "application/json", "[]");
    return;
  }

  File f = LittleFS.open("/datalog.csv", "r");
  if (!f) {
    server.send(500, "application/json", "[]");
    return;
  }

  String header = f.readStringUntil('\n');  // skip header

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    int p4 = line.indexOf(',', p3 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) continue;

    String d    = line.substring(0, p1);
    String t    = line.substring(p1 + 1, p2);
    String c    = line.substring(p2 + 1, p3);
    String fStr = line.substring(p3 + 1, p4);
    String h    = line.substring(p4 + 1);

    String json = "{";
    json += "\"date\":\"" + d + "\",";
    json += "\"time\":\"" + t + "\",";
    json += "\"tempC\":" + c + ",";
    json += "\"tempF\":" + fStr + ",";
    json += "\"hum\":"   + h;
    json += "}";

    if (!first) server.sendContent(",");
    server.sendContent(json);
    first = false;
  }
  server.sendContent("]");
  f.close();
}

// -------------------- WEBSOCKET HANDLER ------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("Client [%u] connected from %s\n", num, ip.toString().c_str());
      String msg = "{\"type\":\"info\",\"message\":\"connected\"}";
      webSocket.sendTXT(num, msg);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("Client [%u] disconnected\n", num);
      break;
    case WStype_TEXT:
      Serial.printf("Received from [%u]: %s\n", num, (char*)payload);
      break;
    default:
      break;
  }
}

// -------------------- FAN & LIGHT LOGIC ------------
void updateFanAndLight(float tC, unsigned long nowMs) {
  // --- FAN with hysteresis ---
  if (!fanOn && tC >= FAN_ON_TEMP_C) {
    fanOn = true;
    digitalWrite(FAN_PIN, LOW);          // ON (active-LOW)
    lastFanChangeStr = getDateTimeStr();
  } else if (fanOn && tC <= FAN_OFF_TEMP_C) {
    fanOn = false;
    digitalWrite(FAN_PIN, HIGH);         // OFF (active-LOW)
    lastFanChangeStr = getDateTimeStr();
  }


  // --- LIGHT with hysteresis + 1-hour timer ---
  if (!lightOn) {
    if (tC <= LIGHT_ON_TEMP_C) {
      lightOn = true;
      digitalWrite(LIGHT_PIN, LOW);      // ON (active-LOW)
      lightOffDeadlineMs = nowMs + LIGHT_ON_DURATION_MS;
      lastLightChangeStr = getDateTimeStr();
    }
  } else {
    bool timeout = (lightOffDeadlineMs != 0) &&
                  ((long)(nowMs - lightOffDeadlineMs) >= 0);

    if (tC >= LIGHT_OFF_TEMP_C || timeout) {
      lightOn = false;
      digitalWrite(LIGHT_PIN, HIGH);     // OFF (active-LOW)
      lightOffDeadlineMs = 0;
      lastLightChangeStr = getDateTimeStr();
    } else if (tC <= LIGHT_ON_TEMP_C) {
      lightOffDeadlineMs = nowMs + LIGHT_ON_DURATION_MS;
    }
  }

}

// -------------------- SENSOR + LOGGING + WS + MQTT -
void sampleAndBroadcast() {
  unsigned long nowMs = millis();
  if (nowMs - lastSampleTimeMs < DATA_RATE_MS) {
    return;
  }
  lastSampleTimeMs = nowMs;

  float newTempC = dht.readTemperature();
  float newHum   = dht.readHumidity();

  if (isnan(newTempC) || isnan(newHum)) {
    Serial.println("Failed to read from DHT sensor.");
    return;
  }

  tempC = newTempC;
  hum   = newHum;
  tempF = tempC * 1.8 + 32.0;

  // Update Fan & Light based on temperature
  updateFanAndLight(tempC, nowMs);

  Serial.print("T(C): ");
  Serial.print(tempC);
  Serial.print("  T(F): ");
  Serial.print(tempF);
  Serial.print("  H(%): ");
  Serial.print(hum);
  Serial.print("  Fan: ");
  Serial.print(fanOn ? "ON" : "OFF");
  Serial.print("  Light: ");
  Serial.println(lightOn ? "ON" : "OFF");

  // Log sample
  logSample(tempC, tempF, hum);

  // WebSocket JSON (local UI)
  unsigned long secondsSinceBoot = nowMs / 1000;
  String wsJson = "{";
  wsJson += "\"tempC\":" + String(tempC, 2) + ",";
  wsJson += "\"tempF\":" + String(tempF, 2) + ",";
  wsJson += "\"hum\":"   + String(hum,   2) + ",";
  wsJson += "\"fanOn\":" + String(fanOn ? "true" : "false") + ",";
  wsJson += "\"fanChanged\":\"" + lastFanChangeStr + "\",";
  wsJson += "\"lightOn\":" + String(lightOn ? "true" : "false") + ",";
  wsJson += "\"lightChanged\":\"" + lastLightChangeStr + "\",";
  wsJson += "\"ts\":"    + String(secondsSinceBoot);
  wsJson += "}";
  webSocket.broadcastTXT(wsJson);

  // MQTT publish (remote dashboard)
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
    time_t now = time(nullptr);

    String payload = "{";
    payload += "\"tempC\":" + String(tempC, 2) + ",";
    payload += "\"tempF\":" + String(tempF, 2) + ",";
    payload += "\"hum\":"   + String(hum,   2) + ",";
    payload += "\"fanOn\":" + String(fanOn ? "true" : "false") + ",";
    payload += "\"fanChanged\":\"" + lastFanChangeStr + "\",";
    payload += "\"lightOn\":" + String(lightOn ? "true" : "false") + ",";
    payload += "\"lightChanged\":\"" + lastLightChangeStr + "\",";

    if (now > 24 * 60 * 60) {
      payload += "\"ts\":" + String((unsigned long)now);
    } else {
      payload += "\"ts\":" + String(secondsSinceBoot);
    }

    payload += "}";

    bool ok = mqttClient.publish(MQTT_TOPIC, payload.c_str());
    if (!ok) {
      Serial.println("MQTT publish failed");
    } else {
      Serial.print("MQTT published: ");
      Serial.println(payload);
    }
  }
}

// -------------------- LOGGING HELPERS --------------
void logSample(float tempC, float tempF, float hum) {
  String dateStr = getDateStr();
  String timeStr = getTimeStr();

  bool onlineTimeValid = (dateStr != "" && timeStr != "");

  if (!onlineTimeValid) {
    dateStr = "0000-00-00";

    unsigned long s = offlineSeconds;
    int hh = (s / 3600) % 24;
    int mm = (s % 3600) / 60;
    int ss = s % 60;

    char buf[9];
    sprintf(buf, "%02d:%02d:%02d", hh, mm, ss);
    timeStr = String(buf);

    offlineSeconds += DATA_RATE_SECONDS;
    offlineLogCount++;

    if (offlineLogCount >= SAMPLES_PER_DAY) {
      Serial.println("Offline log limit reached (~24h). Rebooting...");
      ESP.restart();
    }
  } else {
    offlineSeconds  = 0;
    offlineLogCount = 0;

    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
      int today = tm_info->tm_mday;
      if (lastOnlineDay == -1) {
        lastOnlineDay = today;
      } else if (today != lastOnlineDay) {
        Serial.println("Day changed (midnight). Rebooting...");
        ESP.restart();
      }
    }
  }

  String filename = "/datalog.csv";
  bool newFile = !LittleFS.exists(filename);

  File f = LittleFS.open(filename, "a");
  if (!f) {
    Serial.println("Failed to open /datalog.csv for append.");
    return;
  }

  if (newFile) {
    f.println("date,time,tempC,tempF,hum");
  }

  String line;
  line.reserve(64);
  line += dateStr;
  line += ",";
  line += timeStr;
  line += ",";
  line += String(tempC, 2);
  line += ",";
  line += String(tempF, 2);
  line += ",";
  line += String(hum, 2);
  f.println(line);
  f.close();
}

// -------------------- FS LISTING -------------------
void listFS() {
  Serial.println("Listing LittleFS contents:");
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    Serial.print("  ");
    Serial.print(dir.fileName());
    if (dir.fileSize()) {
      Serial.print("  (");
      Serial.print(dir.fileSize());
      Serial.println(" bytes)");
    } else {
      Serial.println();
    }
  }
}
