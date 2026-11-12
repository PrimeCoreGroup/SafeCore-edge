#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_system.h>

#if __has_include("config.h")
#include "config.h"
#else
#error Copiar config.example.h como config.h y configurar Wi-Fi, UUID, clave local e IP del gateway.
#endif

// Transporte añadido al sketch físico original. No decide ni modifica alarmas.
// La cola es RAM: protege cortes breves de Wi-Fi/gateway, pero no reinicios.
namespace edge {
constexpr uint16_t CAPACITY = 192;
constexpr uint32_t PUBLISH_MS = 10000;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t HTTP_RETRY_MS = 3000;

enum class Sensor : uint8_t { Temperature, Gas, Accelerometer };

struct Reading {
  uint32_t sequence;
  Sensor sensor;
  float value;
  int16_t x, y, z;
  bool digitalAlarm;
};

Reading queue[CAPACITY];
uint16_t head = 0;
uint16_t size = 0;
uint32_t nextSequence = 1;
uint32_t bootNonceHigh = 0;
uint32_t bootNonceLow = 0;
uint32_t lastPublish = 0;
bool lastAlarm = false;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

void enqueue(Sensor sensor, float value, int16_t x = 0, int16_t y = 0,
             int16_t z = 0, bool digitalAlarm = false) {
  portENTER_CRITICAL(&queueMux);
  const bool overflow = size == CAPACITY;
  if (overflow) {
    head = (head + 1) % CAPACITY;
    --size;
  }
  queue[(head + size) % CAPACITY] = {
      nextSequence++, sensor, value, x, y, z, digitalAlarm};
  ++size;
  portEXIT_CRITICAL(&queueMux);
  if (overflow) Serial.println("Cola Edge llena: se descarta la lectura más antigua");
}

bool first(Reading &reading) {
  portENTER_CRITICAL(&queueMux);
  const bool available = size > 0;
  if (available) reading = queue[head];
  portEXIT_CRITICAL(&queueMux);
  return available;
}

void acknowledge(uint32_t sequence) {
  portENTER_CRITICAL(&queueMux);
  if (size > 0 && queue[head].sequence == sequence) {
    head = (head + 1) % CAPACITY;
    --size;
  }
  portEXIT_CRITICAL(&queueMux);
}

int send(const Reading &reading) {
  JsonDocument json;
  json["device_id"] = SAFECORE_DEVICE_ID;
  char eventId[28];
  snprintf(eventId, sizeof(eventId), "%08lX%08lX-%08lX",
           (unsigned long)bootNonceHigh, (unsigned long)bootNonceLow,
           (unsigned long)reading.sequence);
  json["event_id"] = eventId;
  switch (reading.sensor) {
    case Sensor::Temperature:
      json["sensor_type"] = "Temperature";
      json["measured_value"] = reading.value;
      break;
    case Sensor::Gas:
      json["sensor_type"] = "Gas";
      json["measured_value"] = (int)reading.value;
      json["digital_alarm"] = reading.digitalAlarm;
      break;
    case Sensor::Accelerometer:
      json["sensor_type"] = "Accelerometer";
      json["axis_x"] = reading.x;
      json["axis_y"] = reading.y;
      json["axis_z"] = reading.z;
      break;
  }
  String body;
  serializeJson(json, body);

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3000);
  if (!http.begin(client, String(EDGE_BASE_URL) + "/api/v1/sensor-readings")) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", EDGE_DEVICE_API_KEY);
  const int status = http.POST(body);
  if (status == 200 || status == 201)
    Serial.println("Lectura almacenada en gateway");
  else
    Serial.printf("Gateway HTTP %d: %.160s\n", status, http.getString().c_str());
  http.end();
  return status;
}

void networkTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  uint32_t lastWifiTry = 0;
  uint32_t lastHttpFailure = 0;
  bool wasConnected = false;
  for (;;) {
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (wasConnected) Serial.println("Sin Wi-Fi: las alarmas locales siguen activas");
      wasConnected = false;
      if (lastWifiTry == 0 || now - lastWifiTry >= WIFI_RETRY_MS) {
        lastWifiTry = now;
        WiFi.begin(SAFECORE_WIFI_SSID, SAFECORE_WIFI_PASSWORD);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!wasConnected) {
      Serial.println("Wi-Fi conectado; enviando al gateway local");
      wasConnected = true;
    }
    Reading reading;
    if (first(reading) &&
        (lastHttpFailure == 0 || now - lastHttpFailure >= HTTP_RETRY_MS)) {
      const int status = send(reading);
      if (status == 200 || status == 201) {
        acknowledge(reading.sequence);
        lastHttpFailure = 0;
      } else {
        lastHttpFailure = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
} // namespace edge

inline void edgeBegin() {
  edge::bootNonceHigh = esp_random();
  edge::bootNonceLow = esp_random();
  edge::lastPublish = millis();
  xTaskCreatePinnedToCore(edge::networkTask, "SafeCoreGateway", 8192,
                          nullptr, 1, nullptr, 0);
}

inline void edgePublish(float temperatureC, int gasAdc, bool gasDigital,
                        int16_t x, int16_t y, int16_t z, bool alarm) {
  const uint32_t now = millis();
  if (alarm != edge::lastAlarm || now - edge::lastPublish >= edge::PUBLISH_MS) {
    edge::lastPublish = now;
    edge::enqueue(edge::Sensor::Temperature, temperatureC);
    edge::enqueue(edge::Sensor::Gas, (float)gasAdc, 0, 0, 0, gasDigital);
    edge::enqueue(edge::Sensor::Accelerometer, 0, x, y, z);
  }
  edge::lastAlarm = alarm;
}
