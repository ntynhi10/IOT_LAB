#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include "DHT.h"

// =====================================================
// CONFIGURATION
// Copy the values from your existing lab1_supabase(1).ino.
// Do not commit real passwords/API keys to GitHub.
// =====================================================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

#define DHTPIN D2
#define DHTTYPE DHT11
#define RELAY D5

DHT dht(DHTPIN, DHTTYPE);

const char* THINGSPEAK_WRITE_API_KEY = "YOUR_THINGSPEAK_WRITE_API_KEY";
const char* THINGSPEAK_SERVER = "api.thingspeak.com";

const char* SUPABASE_URL =
  "https://YOUR_PROJECT.supabase.co/rest/v1/lab1_sensor_data";
const char* SUPABASE_API_KEY = "YOUR_SUPABASE_PUBLISHABLE_KEY";

// IP of the PC running the Node.js backend.
// Example: http://192.168.1.100:3000
const char* DASHBOARD_SERVER = "http://YOUR_PC_IP:3000";
const char* DEVICE_ID = "ESP8266_LAB1";

const char* ALERT_API_KEY = "YOUR_THINGSPEAK_ALERT_API_KEY";
const char* ALERT_URL = "https://api.thingspeak.com/alerts/send";

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const float TEMPERATURE_THRESHOLD = 30.0;
const unsigned long SEND_INTERVAL = 15000;

unsigned long lastSendTime = 0;
bool alertSent = false;
bool latencySent = false;

// =====================================================
// TIME
// =====================================================
uint64_t utcMillis() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return ((uint64_t)tv.tv_sec * 1000ULL) +
         ((uint64_t)tv.tv_usec / 1000ULL);
}

bool utcTimeReady() {
  return time(nullptr) >= 1700000000;
}

String uint64ToString(uint64_t value) {
  if (value == 0) return "0";

  char buffer[21];
  int i = 20;
  buffer[i] = '\0';

  while (value > 0 && i > 0) {
    buffer[--i] = char('0' + (value % 10));
    value /= 10;
  }

  return String(&buffer[i]);
}

// =====================================================
// THINGSPEAK
// =====================================================
void sendToThingSpeak(float temperature, float humidity) {
  WiFiClient client;

  if (!client.connect(THINGSPEAK_SERVER, 80)) {
    Serial.println("ThingSpeak connection FAILED!");
    return;
  }

  String postStr = "api_key=";
  postStr += THINGSPEAK_WRITE_API_KEY;
  postStr += "&field1=";
  postStr += String(temperature, 2);
  postStr += "&field2=";
  postStr += String(humidity, 2);

  client.println("POST /update HTTP/1.1");
  client.println("Host: api.thingspeak.com");
  client.println("Connection: close");
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.print("Content-Length: ");
  client.println(postStr.length());
  client.println();
  client.print(postStr);

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println("ThingSpeak response TIMEOUT!");
      client.stop();
      return;
    }
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  if (statusLine.indexOf("200 OK") >= 0) {
    Serial.println("ThingSpeak: Data sent successfully.");
  } else {
    Serial.print("ThingSpeak response: ");
    Serial.println(statusLine);
  }

  client.stop();
}

// =====================================================
// SUPABASE
// =====================================================
void sendToSupabase(float temperature, float humidity) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // classroom/demo shortcut

  HTTPClient https;

  if (!https.begin(secureClient, SUPABASE_URL)) {
    Serial.println("Supabase connection FAILED!");
    return;
  }

  https.addHeader("apikey", SUPABASE_API_KEY);
  https.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"device_id\":\"";
  json += DEVICE_ID;
  json += "\",\"temperature\":";
  json += String(temperature, 2);
  json += ",\"humidity\":";
  json += String(humidity, 2);
  json += "}";

  int httpCode = https.POST(json);

  Serial.print("Supabase HTTP code: ");
  Serial.println(httpCode);

  if (httpCode >= 200 && httpCode < 300) {
    Serial.println("Supabase: Data sent successfully.");
  } else {
    Serial.println("Supabase: Data send FAILED!");
    Serial.println(https.getString());
  }

  https.end();
}

// =====================================================
// LATENCY: t2 -> BACKEND
// Only called for a NEW alert episode (temperature >= 30).
// Latency is NOT stored in Supabase.
// =====================================================
void sendLatencyCompletion(const String& eventId, uint64_t t3) {
  WiFiClient client;
  HTTPClient http;

  String url = String(DASHBOARD_SERVER) + "/api/latency/complete";

  if (!http.begin(client, url)) {
    Serial.println("Latency completion connection FAILED!");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"event_id\":\"";
  json += eventId;
  json += "\",\"t3\":";
  json += uint64ToString(t3);
  json += "}";

  int code = http.POST(json);

  Serial.print("Latency completion HTTP code: ");
  Serial.println(code);

  http.end();
}

bool sendLatencyTelemetry(float temperature, float humidity,
                          uint64_t t0, uint64_t t1) {
  if (!utcTimeReady()) {
    Serial.println("Latency skipped: UTC time is not synchronized.");
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String(DASHBOARD_SERVER) + "/api/latency";

  if (!http.begin(client, url)) {
    Serial.println("Latency backend connection FAILED!");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  // t2 = immediately before the alert telemetry request starts.
  uint64_t t2 = utcMillis();
  String eventId = String(DEVICE_ID) + "_" + uint64ToString(t2);

  String json = "{";
  json += "\"event_id\":\"";
  json += eventId;
  json += "\",\"device_id\":\"";
  json += DEVICE_ID;
  json += "\",\"temperature\":";
  json += String(temperature, 2);
  json += ",\"humidity\":";
  json += String(humidity, 2);
  json += ",\"fire_suspected\":true";
  json += ",\"t0\":";
  json += uint64ToString(t0);
  json += ",\"t1\":";
  json += uint64ToString(t1);
  json += ",\"t2\":";
  json += uint64ToString(t2);
  json += "}";

  Serial.println("=== LATENCY EVENT ===");
  Serial.println("event_id: " + eventId);
  Serial.println("t0: " + uint64ToString(t0));
  Serial.println("t1: " + uint64ToString(t1));
  Serial.println("t2: " + uint64ToString(t2));

  int httpCode = http.POST(json);

  // t3 = request finished.
  uint64_t t3 = utcMillis();

  Serial.println("t3: " + uint64ToString(t3));
  Serial.print("Latency backend HTTP code: ");
  Serial.println(httpCode);

  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    sendLatencyCompletion(eventId, t3);
    return true;
  }

  return false;
}

// =====================================================
// EMAIL ALERT
// =====================================================
void sendEmailAlert(float temperature) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient https;

  if (!https.begin(secureClient, ALERT_URL)) {
    Serial.println("Cannot connect to ThingSpeak Alerts API!");
    return;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("ThingSpeak-Alerts-API-Key", ALERT_API_KEY);

  String json = "{";
  json += "\"subject\":\"CANH BAO NHIET DO CAO\",";
  json += "\"body\":\"CANH BAO! Nhiet do vuot nguong 30 C. Nhiet do hien tai: ";
  json += String(temperature, 2);
  json += " C.\"";
  json += "}";

  int httpCode = https.POST(json);

  Serial.print("Alerts API HTTP code: ");
  Serial.println(httpCode);

  if (httpCode == 202) {
    Serial.println("EMAIL ALERT ACCEPTED.");
  } else {
    Serial.println("EMAIL ALERT FAILED.");
    Serial.println(https.getString());
  }

  https.end();
}

// =====================================================
// RELAY + ALERT STATE
// =====================================================
void controlRelay(float temperature, float humidity,
                  bool fireSuspected,
                  uint64_t t0, uint64_t t1) {

  if (fireSuspected) {
    digitalWrite(RELAY, HIGH);

    Serial.println("WARNING! HIGH TEMPERATURE!");
    Serial.println("Relay: ON");

    // One latency event + one email per high-temperature episode.
    if (!alertSent) {
      if (!latencySent) {
        latencySent =
          sendLatencyTelemetry(temperature, humidity, t0, t1);
      }

      sendEmailAlert(temperature);
      alertSent = true;
    }
  } else {
    digitalWrite(RELAY, LOW);

    Serial.println("Relay: OFF");

    // Below threshold => new alert episode is allowed later.
    alertSent = false;
    latencySent = false;
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, LOW);

  dht.begin();

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP8266 DHT11 + RELAY + LATENCY");
  Serial.println("================================");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // UTC time base for t0..t3.
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  Serial.print("Synchronizing UTC");
  time_t now = time(nullptr);
  unsigned long start = millis();

  while (now < 1700000000 && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }

  Serial.println();

  if (now >= 1700000000)
    Serial.println("UTC synchronized.");
  else
    Serial.println("WARNING: UTC sync failed.");

  Serial.println("System ready.");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // t0 = sensor sample time.
  uint64_t t0 = utcMillis();

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("ERROR: Failed to read DHT11!");
    digitalWrite(RELAY, LOW);
    delay(2000);
    return;
  }

  // Local decision.
  bool fireSuspected =
    (temperature >= TEMPERATURE_THRESHOLD);

  // t1 = local decision completed.
  uint64_t t1 = utcMillis();

  Serial.println("--------------------------------");
  Serial.print("Temperature: ");
  Serial.print(temperature, 2);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity, 2);
  Serial.println(" %");

  controlRelay(
    temperature,
    humidity,
    fireSuspected,
    t0,
    t1
  );

  // Normal telemetry continues regardless of alert state.
  if (millis() - lastSendTime >= SEND_INTERVAL ||
      lastSendTime == 0) {

    sendToThingSpeak(temperature, humidity);
    sendToSupabase(temperature, humidity);

    lastSendTime = millis();
  }

  delay(2000);
}
