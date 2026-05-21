#include <Wire.h>
#include <SensirionI2CSgp41.h>
#include <SensirionGasIndexAlgorithm.h>
#include <sps30.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── Configuration ─────────────────────────────────────────────────────────────
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqttHost = "YOUR_SERVER_IP";
const int   mqttPort = 1883;
// ──────────────────────────────────────────────────────────────────────────────

WiFiClient espClient;
PubSubClient mqttClient(espClient);

SensirionI2CSgp41 sgp41;
SPS30 sps30;
SensirionGasIndexAlgorithm vocAlgorithm(1);
SensirionGasIndexAlgorithm noxAlgorithm(2);

// ── MQTT reconnect ────────────────────────────────────────────────────────────
void reconnectMQTT() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 10) {
    Serial.println("Connecting to MQTT...");
    if (mqttClient.connect("esp32-air")) {
      Serial.println("MQTT connected");
    } else {
      Serial.printf("MQTT failed (rc=%d), retry in 5s\n", mqttClient.state());
      delay(5000);
      attempts++;
    }
  }
}

// ── Diagnostics ───────────────────────────────────────────────────────────────
void sendDiagnostics() {
  String diag = "{\"heap\":"    + String(ESP.getFreeHeap()) +
                ",\"rssi\":"    + String(WiFi.RSSI()) +
                ",\"uptime\":"  + String(millis() / 1000) +
                ",\"wifi\":"    + String(WiFi.status()) +
                ",\"mqtt\":"    + String(mqttClient.connected()) +
                ",\"cpu_temp\":" + String(temperatureRead()) + "}";
  mqttClient.publish("air/diagnostics", diag.c_str());
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout reset

  Serial.begin(115200);
  delay(5000); // give sensors time to power up on cold boot

  // I2C with timeout to prevent bus hangs
  Wire.begin(21, 22); // SDA, SCL
  Wire.setTimeOut(1000);

  sgp41.begin(Wire);
  sps30.begin(I2C_COMMS);
  sps30.start();

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Connect to MQTT
  mqttClient.setServer(mqttHost, mqttPort);
  reconnectMQTT();

  // Send boot reason (useful for diagnosing unexpected restarts)
  String bootReason = "{\"boot_reason\":" + String(esp_reset_reason()) + "}";
  mqttClient.publish("air/diagnostics", bootReason.c_str());
  Serial.println("Boot reason sent: " + bootReason);

  // Hardware watchdog — restart ESP32 if loop() hangs for >30 seconds
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms  = 30000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset(); // feed the watchdog

  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500);
      tries++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi failed, restarting...");
      esp_restart();
    }
  }

  // MQTT reconnect
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Send diagnostics every cycle
  sendDiagnostics();

  // ── SGP41 ────────────────────────────────────────────────────────────────
  uint16_t srawVoc, srawNox;
  sgp41.measureRawSignals(0x8000, 0x8000, srawVoc, srawNox);
  int32_t vocIndex = vocAlgorithm.process(srawVoc);
  int32_t noxIndex = noxAlgorithm.process(srawNox);

  // ── SPS30 ────────────────────────────────────────────────────────────────
  struct sps_values val;
  sps30.GetValues(&val);

  if (val.MassPM2 == 0.0) {
    Serial.println("SPS30 not ready, reinitializing...");
    sps30.begin(I2C_COMMS);
    sps30.start();
    delay(5000);
    return;
  }

  // ── Publish sensor data ───────────────────────────────────────────────────
  String payload = "{\"pm25\":"  + String(val.MassPM2) +
                   ",\"pm10\":"  + String(val.MassPM10) +
                   ",\"pm1\":"   + String(val.MassPM1) +
                   ",\"pm4\":"   + String(val.MassPM4) +
                   ",\"voc\":"   + String(vocIndex) +
                   ",\"nox\":"   + String(noxIndex) + "}";

  mqttClient.publish("air/sensor", payload.c_str());
  Serial.println("Published: " + payload);

  delay(1000);
}
