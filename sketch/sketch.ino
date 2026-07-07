#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <DHT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Wi-Fi credentials
const char* WIFI_SSID     = "IKI";  
const char* WIFI_PASSWORD = "12345678";             

// MQTT broker (HiveMQ Cloud)
const char* MQTT_BROKER   = "10.196.3.232"; 
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "smartfarm-esp32";
const char* MQTT_TOPIC    = "Group6_SmartFarm";

// Firebase fallback
const char* FIREBASE_URL  = "https://smartfarmiot-76187-default-rtdb.firebaseio.com/";
const char* FIREBASE_AUTH = "LCIE3zkzD3ZSscEhXYGM0os3pDoK51HesZo218xE";

// Pin definitions
#define SOIL_PIN       34    // Analog input — capacitive soil moisture
#define LDR_PIN        33 
#define DHT_PIN         4    // Digital — DHT11 data
#define DHT_TYPE    DHT11
#define SDA_PIN        21    // OLED SDA
#define SCL_PIN       22    // OLED SCL

// LDR lux mapping
#define LDR_MAX_RAW  4095

// OLED display
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDRESS 0x3C
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Sensor objects
DHT    dht(DHT_PIN, DHT_TYPE);

// Network objects
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// Timing 
unsigned long lastSoilRead  = 0;
unsigned long lastDHTRead   = 0;
unsigned long lastLightRead = 0;
unsigned long lastPublish   = 0;

const unsigned long SOIL_INTERVAL  = 600000UL;  // 10 min
const unsigned long DHT_INTERVAL   = 300000UL;  // 5 min
const unsigned long LIGHT_INTERVAL = 10000UL;  // 10 sec
const unsigned long PUBLISH_INTERVAL = 10000UL; // 10 sec 

// Sensor value store
float soilMoisturePct = 0.0;
float temperature     = 0.0;
float humidity        = 0.0;
float lightLevel      = 0.0;
bool  mqttConnected   = false;

// Alert thresholds
const float SOIL_LOW_PCT   = 30.0;
const float TEMP_HIGH_C    = 35.0;
const float HUMID_LOW_PCT  = 40.0;
const float LIGHT_LOW_LEVEL = 30.0;

// Soil moisture calibration
//   AIR_VALUE   = raw ADC reading when sensor is in open air (dry)
//   WATER_VALUE = raw ADC reading when sensor is fully submerged
const int AIR_VALUE   = 3200;
const int WATER_VALUE = 1400;

// SETUP
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  initDisplay();
  initSensors();
  connectWiFi();
  connectMQTT();

  Serial.println("[SmartFarm] System ready.");
}

// LOOP
void loop() {
  unsigned long now = millis();

  // Keep MQTT alive
  if (!mqtt.connected()) {
    mqttConnected = false;
    connectMQTT();
  }
  mqtt.loop();

  // Read soil moisture every 10 min 
  if (now - lastSoilRead >= SOIL_INTERVAL || lastSoilRead == 0) {
    readSoilMoisture();
    lastSoilRead = now;
  }

  // Read DHT11 every 5 min
  if (now - lastDHTRead >= DHT_INTERVAL || lastDHTRead == 0) {
    readDHT();
    lastDHTRead = now;
  }

  // Read LDR every 15 min
  if (now - lastLightRead >= LIGHT_INTERVAL || lastLightRead == 0) {
    readLight();
    lastLightRead = now;
  }

  // Publish every PUBLISH_INTERVAL
  if (now - lastPublish >= PUBLISH_INTERVAL || lastPublish == 0) {
    updateDisplay();
    publishReadings();
    printSerial();
    lastPublish = now;
  }
}

// SENSOR READS
void readSoilMoisture() {
  int raw = analogRead(SOIL_PIN);
  // Map raw ADC to 0–100% (higher raw = drier soil for capacitive sensor)
  soilMoisturePct = map(raw, AIR_VALUE, WATER_VALUE, 0, 100);
  soilMoisturePct = constrain(soilMoisturePct, 0.0, 100.0);
  Serial.printf("[Soil] Raw: %d → %.1f%%\n", raw, soilMoisturePct);
}

void readDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    temperature = t;
    humidity    = h;
    Serial.printf("[DHT11] Temp: %.1f°C  Humidity: %.1f%%\n", temperature, humidity);
  } else {
    Serial.println("[DHT11] Read failed — retaining last values.");
  }
}

void readLight() {
  int raw = analogRead(LDR_PIN);

  // Convert ADC value to a percentage (0–100%)
  lightLevel = map(raw, LDR_MAX_RAW, 0, 0, 100);
  lightLevel = constrain(lightLevel, 0.0, 100.0);

  Serial.printf("[LDR] Raw: %d -> Light Level: %.1f%%\n", raw, lightLevel);
}

// PUBLISH — MQTT primary, Firebase fallback
void publishReadings() {
  // Build JSON payload
  StaticJsonDocument<256> doc;
  doc["soil_moisture"] = soilMoisturePct;
  doc["temperature"]   = temperature;
  doc["humidity"]      = humidity;
  doc["light_level"] = lightLevel;
  doc["timestamp"]     = millis();

  // Alerts
  JsonObject alerts = doc.createNestedObject("alerts");
  alerts["irrigate"]   = soilMoisturePct < SOIL_LOW_PCT;
  alerts["heat_stress"] = temperature    > TEMP_HIGH_C;
  alerts["dry_air"]    = humidity        < HUMID_LOW_PCT;
  alerts["poor_light"] = lightLevel      < LIGHT_LOW_LEVEL;

  char payload[256];
  serializeJson(doc, payload);

  // Primary: publish via MQTT
  if (mqtt.connected()) {
    bool ok = mqtt.publish(MQTT_TOPIC, payload, true); // retained message
    mqttConnected = ok;
    if (ok) {
      Serial.println("[MQTT] Published successfully.");
      return; // done — no need for fallback
    }
  }

  // Fallback: POST to Firebase REST API
  Serial.println("[MQTT] Not connected — falling back to Firebase.");
  postToFirebase(payload);
}

void postToFirebase(const char* payload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firebase] No Wi-Fi — skipping.");
    return;
  }
  HTTPClient http;
  String url = String(FIREBASE_URL) + "?auth=" + FIREBASE_AUTH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  if (code > 0) {
    Serial.printf("[Firebase] POST response: %d\n", code);
  } else {
    Serial.printf("[Firebase] POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// OLED DISPLAY
void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  // Header
  display.setCursor(0, 0);
  display.println("-- SmartFarm IoT --");
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);

  // Sensor readings
  display.setCursor(0, 13);
  display.printf("Soil : %.1f%%\n", soilMoisturePct);
  display.printf("Temp : %.1fC\n",  temperature);
  display.printf("Humid: %.1f%%\n", humidity);
  display.printf("Light: %.0f%%\n", lightLevel);

  // Status bar
  display.drawLine(0, 54, 127, 54, SH110X_WHITE);
  display.setCursor(0, 56);
  display.print(mqttConnected ? "MQTT: OK" : "MQTT: FB fallback");

  display.display();
}

// SERIAL DEBUG
void printSerial() {
  Serial.println("┌─────────────────────────────┐");
  Serial.println("│      SmartFarm Readings      │");
  Serial.println("├─────────────────────────────┤");
  Serial.printf( "│ Soil moisture : %6.1f %%    │\n", soilMoisturePct);
  Serial.printf( "│ Temperature   : %6.1f °C   │\n", temperature);
  Serial.printf( "│ Humidity      : %6.1f %%    │\n", humidity);
  Serial.printf("│ Light Level   : %6.1f %% │\n", lightLevel);
  Serial.println("├─────────────────────────────┤");
  Serial.printf( "│ MQTT status   : %s\n", mqttConnected ? "connected     │" : "offline→FB    │");
  Serial.println("└─────────────────────────────┘");
}

// INIT HELPERS
void initDisplay() {
  if (!display.begin( OLED_ADDRESS, true)) {
    Serial.println("[OLED] Init failed — check wiring.");
    while (true); // halt
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(20, 24);
  display.println("SmartFarm IoT");
  display.setCursor(20, 36);
  display.println("Initialising...");
  display.display();
  delay(1500);
}

void initSensors() {
  dht.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
  Serial.println("[Sensors] DHT11 and LDR initialised.");
}

void connectWiFi() {
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed — running in offline mode.");
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(512);

  Serial.print("[MQTT] Connecting to broker");
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    if (mqtt.connect(MQTT_CLIENT)) {
      mqttConnected = true;
      Serial.println("\n[MQTT] Connected.");
    } else {
      Serial.printf(".\n[MQTT] Failed rc=%d — retrying\n", mqtt.state());
      delay(2000);
      attempts++;
    }
  }
  if (!mqtt.connected()) {
    Serial.println("[MQTT] Could not connect — Firebase fallback active.");
  }
}